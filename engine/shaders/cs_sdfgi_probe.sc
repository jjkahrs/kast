#include <bgfx_compute.sh>

// SDFGI probes (sdfgi.h SdfgiRenderer::update): one thread per probe of every cascade, every frame. Marches u_sdfgiParams
// rays (a spherical Fibonacci set turned by u_sdfgiRotation, new each frame) from the probe through the SDF: a hit takes
// the lit cell's light (cs_sdfgi_light.sc), a ray leaving the cascades (or out of steps) the sky's light along it (u_sdfgiSky,
// irradiance / pi harmonics as fs_mesh.sc u_envSh: the flat ambient in c0 without a sky). Their L1 harmonics, a0 the
// mean radiance and a1 3 x the mean of radiance x direction (so radiance(d) = a0 + a1 . d and irradiance / pi at n =
// a0 + 2/3 a1 . n), blend into last frame's by the hysteresis (taken whole if it had none: a = -1). Texel 0's a: 1, or 0
// for a probe inside geometry (its rays all start in it), which the meshes skip.
IMAGE3D_RO(s_sdf, rgba16f, 0);
IMAGE3D_RO(s_light, rgba16f, 1);
IMAGE3D_RO(s_probesIn, rgba16f, 2);
IMAGE3D_WO(s_probesOut, rgba16f, 3);

#include "sdfgi.sh"

// [0] x: rays, y: steps, z: hysteresis, w: frame; [1] x: bounce feedback, y: lights
uniform vec4 u_sdfgiParams[2];
uniform vec4 u_sdfgiSky[9];      // Sh9 as fs_mesh.sc u_envSh
uniform vec4 u_sdfgiRotation[3]; // rows of this frame's turn of the ray set

vec3 sky(vec3 n)
{
    vec3 e = u_sdfgiSky[0].rgb + u_sdfgiSky[1].rgb * n.y + u_sdfgiSky[2].rgb * n.z + u_sdfgiSky[3].rgb * n.x
           + u_sdfgiSky[4].rgb * (n.x * n.y) + u_sdfgiSky[5].rgb * (n.y * n.z) + u_sdfgiSky[6].rgb * (3.0 * n.z * n.z - 1.0)
           + u_sdfgiSky[7].rgb * (n.x * n.z) + u_sdfgiSky[8].rgb * (n.x * n.x - n.y * n.y);
    return max(e, vec3_splat(0.0));
}

NUM_THREADS(64, 1, 1)
void main()
{
    vec3 dims = u_sdfgiGrid.xyz / float(PROBE_CELLS);
    float perCascade = dims.x * dims.y * dims.z;
    float index = float(gl_GlobalInvocationID.x);
    if (index >= perCascade * u_sdfgiGrid.w)
        return;
    int c = int(index / perCascade);
    float i = index - float(c) * perCascade;
    vec3 t = vec3(i - dims.x * floor(i / dims.x), floor(i / dims.x) - dims.y * floor(i / (dims.x * dims.y)),
                  floor(i / (dims.x * dims.y)));
    vec3 first = u_sdfgiCascades[c].xyz / float(PROBE_CELLS);
    vec3 q = first + sdfgiWrap(t - first, dims); // the global probe at this texel
    float cell = u_sdfgiCascades[c].w;
    vec3 o = q * (cell * float(PROBE_CELLS));
    ivec3 texel = ivec3(int(t.x) * 4, int(t.y), int(t.z) + c * int(dims.z));

    float inside = imageLoad(s_sdf, sdfgiTexel(q * float(PROBE_CELLS), c)).a < -0.25 ? 1.0 : 0.0;
    float rays = u_sdfgiParams[0].x;
    vec3 a0 = vec3_splat(0.0);
    vec3 ar = vec3_splat(0.0);
    vec3 ag = vec3_splat(0.0);
    vec3 ab = vec3_splat(0.0);
    for (int r = 0; r < 64; ++r) {
        if (float(r) >= rays)
            break;
        float z = 1.0 - (2.0 * float(r) + 1.0) / rays;
        float s = sqrt(max(1.0 - z * z, 0.0));
        float phi = float(r) * 2.39996323; // the golden angle
        vec3 f = vec3(s * cos(phi), z, s * sin(phi));
        vec3 d = vec3(dot(u_sdfgiRotation[0].xyz, f), dot(u_sdfgiRotation[1].xyz, f), dot(u_sdfgiRotation[2].xyz, f));
        ivec3 hit;
        vec3 l = sdfgiMarch(o, d, cell, 1e6, u_sdfgiParams[0].y, c, hit) ? imageLoad(s_light, hit).rgb : sky(d);
        a0 += l;
        ar += l.r * d;
        ag += l.g * d;
        ab += l.b * d;
    }
    a0 /= rays;
    float k = 3.0 / rays;
    vec4 now[4];
    now[0] = vec4(a0, 1.0 - inside);
    now[1] = vec4(ar * k, 0.0);
    now[2] = vec4(ag * k, 0.0);
    now[3] = vec4(ab * k, 0.0);
    float keep = imageLoad(s_probesIn, texel).a < -0.5 ? 0.0 : u_sdfgiParams[0].z;
    for (int j = 0; j < 4; ++j) {
        vec4 old = imageLoad(s_probesIn, texel + ivec3(j, 0, 0));
        vec4 v = vec4(mix(now[j].xyz, old.xyz, keep), now[j].w);
        imageStore(s_probesOut, texel + ivec3(j, 0, 0), v);
    }
}
