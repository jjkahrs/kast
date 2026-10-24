#include <bgfx_compute.sh>

// SDFGI light (sdfgi.h SdfgiRenderer::update): one thread per cell of every cascade, every frame. A cell on a surface
// (under a cell from it) is lit where the surface crosses it, with the SDF's gradient as its normal: by every light of
// u_sdfgiLights (gi_lights.sh), each shadowed by marching the SDF toward it, plus bounce feedback times its cascade's
// probes' light there (last frame's: the light bounces once more each frame), times its albedo, plus its emission. The
// other cells get black.
IMAGE3D_RO(s_sdf, rgba16f, 0);    // rgb emission, a distance (cells)
IMAGE3D_RO(s_albedo, rgba8, 1);
IMAGE3D_RO(s_probes, rgba16f, 2); // as cs_sdfgi_probe.sc writes them
IMAGE3D_WO(s_lightOut, rgba16f, 3);

#define MAX_LIGHTS 16 // kMaxVoxelGiLights in voxel_gi.h

#include "sdfgi.sh"
#include "gi_lights.sh"

// [0] x: rays, y: steps, z: hysteresis, w: frame; [1] x: bounce feedback, y: lights
uniform vec4 u_sdfgiParams[2];
uniform vec4 u_sdfgiLights[MAX_LIGHTS * 4]; // packVoxelGiLights in voxel_gi.h says how (no shadow maps: the SDF's)

float distanceAt(vec3 g, int c)
{
    vec3 o = u_sdfgiCascades[c].xyz;
    g = clamp(g, o, o + u_sdfgiGrid.xyz - vec3_splat(1.0)); // one-sided at the cascade's faces
    return imageLoad(s_sdf, sdfgiTexel(g, c)).a;
}

// Cascade c's probes' diffuse light (irradiance / pi) at p, unit normal n: sdfgiProbeWeights in sdfgi.cpp, as
// fs_mesh.sc kastSdfgiCascade samples them.
vec3 probeLight(int c, vec3 p, vec3 n)
{
    float spacing = u_sdfgiCascades[c].w * float(PROBE_CELLS);
    vec3 dims = u_sdfgiGrid.xyz / float(PROBE_CELLS);
    vec3 first = u_sdfgiCascades[c].xyz / float(PROBE_CELLS);
    vec3 local = clamp((p + n * (NORMAL_BIAS * spacing)) / spacing - first, vec3_splat(0.0), dims - vec3_splat(1.001));
    vec3 base = floor(local);
    vec3 f = local - base;
    vec3 sum = vec3_splat(0.0);
    float total = 0.0;
    for (int i = 0; i < 8; ++i) {
        vec3 corner = vec3(float(i - (i / 2) * 2), float((i / 2) - (i / 4) * 2), float(i / 4));
        vec3 q = first + base + corner;
        vec3 t = sdfgiWrap(q, dims);
        ivec3 texel = ivec3(int(t.x) * 4, int(t.y), int(t.z) + c * int(dims.z));
        vec4 a0 = imageLoad(s_probes, texel);
        if (a0.a < 0.5)
            continue; // inside geometry, or no history yet
        vec3 tri = mix(vec3_splat(1.0) - f, f, corner);
        vec3 toProbe = normalize(q * spacing - p + vec3_splat(1e-5));
        float back = (dot(toProbe, n) + 1.0) * 0.5;
        float w = tri.x * tri.y * tri.z * (back * back + 0.05);
        vec3 r = imageLoad(s_probes, texel + ivec3(1, 0, 0)).xyz;
        vec3 g = imageLoad(s_probes, texel + ivec3(2, 0, 0)).xyz;
        vec3 b = imageLoad(s_probes, texel + ivec3(3, 0, 0)).xyz;
        sum += w * (a0.rgb + (2.0 / 3.0) * vec3(dot(r, n), dot(g, n), dot(b, n)));
        total += w;
    }
    return total > 0.0 ? max(sum / total, vec3_splat(0.0)) : vec3_splat(0.0);
}

NUM_THREADS(4, 4, 4)
void main()
{
    ivec3 texel = ivec3(gl_GlobalInvocationID);
    vec3 grid = u_sdfgiGrid.xyz;
    if (float(texel.x) >= grid.x || float(texel.y) >= grid.y || float(texel.z) >= grid.z * u_sdfgiGrid.w)
        return;
    int c = int(float(texel.z) / grid.z);
    vec4 sdf = imageLoad(s_sdf, texel);
    if (abs(sdf.a) >= 1.0) {
        imageStore(s_lightOut, texel, vec4_splat(0.0));
        return;
    }
    vec3 g = sdfgiCellOf(texel, c);
    float cell = u_sdfgiCascades[c].w;
    vec3 grad = vec3(distanceAt(g + vec3(1.0, 0.0, 0.0), c) - distanceAt(g - vec3(1.0, 0.0, 0.0), c),
                     distanceAt(g + vec3(0.0, 1.0, 0.0), c) - distanceAt(g - vec3(0.0, 1.0, 0.0), c),
                     distanceAt(g + vec3(0.0, 0.0, 1.0), c) - distanceAt(g - vec3(0.0, 0.0, 1.0), c));
    vec3 n = dot(grad, grad) > 1e-6 ? normalize(grad) : vec3(0.0, 1.0, 0.0);
    vec3 p = (g + 0.5) * cell - n * (sdf.a * cell); // onto the surface
    vec3 albedo = imageLoad(s_albedo, texel).rgb;
    vec3 e = vec3_splat(0.0);
    for (int l = 0; l < MAX_LIGHTS; ++l) {
        if (float(l) >= u_sdfgiParams[1].y)
            break;
        vec4 a = u_sdfgiLights[l * 4];
        vec4 b = u_sdfgiLights[l * 4 + 1];
        vec3 light = giLightIrradiance(a, b, u_sdfgiLights[l * 4 + 2], u_sdfgiLights[l * 4 + 3], p, n);
        if (dot(light, light) <= 0.0)
            continue;
        vec3 toLight = a.w > 3.5 ? -a.xyz : a.xyz - p;
        float reach = a.w > 3.5 ? 1e6 : length(toLight) - cell;
        ivec3 hit;
        if (sdfgiMarch(p + n * cell, normalize(toLight), cell, reach, u_sdfgiParams[0].y, c, hit))
            continue;
        e += light;
    }
    e += u_sdfgiParams[1].x * probeLight(c, p, n);
    imageStore(s_lightOut, texel, vec4(albedo * e + sdf.rgb, 1.0));
}
