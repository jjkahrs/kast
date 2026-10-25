#include <bgfx_compute.sh>

// Volumetric fog in-scatter (volumetric_fog.h VolumetricFog::update): one thread per froxel. The light its medium
// (cs_volfog_inject.sc) scatters toward the eye: each light of u_volFogLights (packVoxelGiLights in voxel_gi.h; the
// sun through its cascades, point and spot lights through their shadow maps when they cast shadows, area lights left
// out) times the Henyey-Greenstein phase, plus the ambient (isotropic: the phase integrates to 1), times the
// scattering. Then blended with the last frame's at the same world point (reprojected by its view-projection) by the
// history's weight. rgb in-scattered radiance per meter, a extinction.
IMAGE3D_RO(s_density, rgba16f, 0);
SAMPLER3D(s_history, 1);           // last frame's output
IMAGE3D_WO(s_scatter, rgba16f, 2);
SAMPLER2DSHADOW(s_csm, 3);           // the shadow atlas as fs_mesh.sc's s_csm
SAMPLERCUBESHADOW(s_pointShadow0, 4); // the shadowed point light cubes as fs_mesh.sc's
SAMPLERCUBESHADOW(s_pointShadow1, 5);
SAMPLERCUBESHADOW(s_pointShadow2, 6);
SAMPLERCUBESHADOW(s_pointShadow3, 7);

#define MAX_LIGHTS 16           // kMaxVoxelGiLights in voxel_gi.h
#define MAX_CASCADES 4          // kMaxShadowCascades in scene.h
#define POINT_SHADOW_NEAR 0.05  // kPointShadowNear in shadow.h

#include "volumetric_fog.sh"
#include "gi_lights.sh" // falloff, spotCone

uniform mat4 u_volFogPrev; // the last frame's view-projection
uniform vec4 u_volFogLights[MAX_LIGHTS * 4];
uniform mat4 u_csmMtx[8];  // as fs_mesh.sc's
uniform vec4 u_csmBias[MAX_CASCADES];

// The sun's shadow at p, as cs_voxelgi_inject.sc sunShadow (the first cascade holding it; lit past them).
float sunShadow(vec3 p)
{
    for (int c = 0; c < MAX_CASCADES; ++c) {
        if (float(c) >= u_volFogGrid[5].w)
            break;
        vec3 coord = mul(u_csmMtx[c], vec4(p, 1.0)).xyz;
        float column = float(c - (c / 2) * 2);
        vec2 lo = vec2(0.5 * column, 0.25 * float(c / 2));
        vec2 hi = lo + vec2(0.5, 0.25);
        if (coord.x > lo.x && coord.y > lo.y && coord.x < hi.x && coord.y < hi.y && coord.z > 0.0 && coord.z < 1.0)
            return shadow2D(s_csm, vec3(coord.xy, coord.z - u_csmBias[c].y));
    }
    return 1.0;
}

// As cs_voxelgi_inject.sc pointShadow, without a normal.
float pointShadow(int cube, vec3 toPoint, float range, float bias)
{
    vec3 a = abs(toPoint);
    float m = max(max(a.x, a.y), a.z) - bias;
    float z = range / (range - POINT_SHADOW_NEAR) - range * POINT_SHADOW_NEAR / ((range - POINT_SHADOW_NEAR) * max(m, 1e-4));
    vec4 coord = vec4(toPoint, z);
    if (cube == 0)
        return shadowCube(s_pointShadow0, coord);
    if (cube == 1)
        return shadowCube(s_pointShadow1, coord);
    if (cube == 2)
        return shadowCube(s_pointShadow2, coord);
    return shadowCube(s_pointShadow3, coord);
}

// As cs_voxelgi_inject.sc tileShadow, without a normal: a spot light's atlas tile (4..7); lit outside it.
float tileShadow(float tile, vec3 p, vec3 toLight, float bias)
{
    vec4 q = mul(u_csmMtx[int(tile)], vec4(p + toLight * bias, 1.0));
    if (q.w <= 0.0)
        return 1.0;
    vec3 coord = q.xyz / q.w;
    float i = tile - float(MAX_CASCADES);
    vec2 lo = vec2(0.5 * (i - 2.0 * floor(i / 2.0)), 0.25 * (2.0 + floor(i / 2.0)));
    vec2 hi = lo + vec2(0.5, 0.25);
    if (coord.x < lo.x || coord.y < lo.y || coord.x > hi.x || coord.y > hi.y || coord.z >= 1.0)
        return 1.0;
    return shadow2D(s_csm, coord);
}

// Light i's radiance at p, shadowed, times the phase toward the eye (toEye unit).
vec3 lightAt(int i, vec3 p, vec3 toEye)
{
    vec4 a = u_volFogLights[i * 4];
    vec4 b = u_volFogLights[i * 4 + 1];
    vec4 c = u_volFogLights[i * 4 + 2];
    vec4 d = u_volFogLights[i * 4 + 3];
    float g = u_volFogGrid[4].w;
    if (a.w > 3.5) { // directional: a.xyz the way it shines
        vec3 e = b.rgb * kastHenyeyGreenstein(dot(a.xyz, toEye), g);
        if (d.z > 7.5)
            e *= sunShadow(p);
        return e;
    }
    if (a.w > 1.5)
        return vec3_splat(0.0); // ponytail: area lights scatter nothing; as a point light at the rect if fog needs them
    vec3 toLight = a.xyz - p;
    float dist = length(toLight);
    vec3 l = toLight / max(dist, 1e-4);
    vec3 e = b.rgb * falloff(dist, b.w);
    if (a.w > 0.5)
        e *= spotCone(dot(-l, c.xyz), d.x, c.w);
    if (dot(e, e) <= 0.0)
        return e;
    if (d.z >= 0.0)
        e *= d.z < 3.5 ? pointShadow(int(d.z), -toLight, b.w, d.w) : tileShadow(d.z, p, l, d.w);
    return e * kastHenyeyGreenstein(dot(-l, toEye), g);
}

NUM_THREADS(4, 4, 4)
void main()
{
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (float(cell.y) >= FROXELS_Y)
        return;
    vec3 p = kastFroxelPosition(cell);
    vec4 medium = imageLoad(s_density, cell);
    vec3 light = vec3_splat(0.0);
    if (medium.a > 0.0) {
        vec3 toEye = -normalize(kastFroxelRay((vec2(cell.xy) + 0.5 + u_volFogGrid[9].xy) / vec2(FROXELS_X, FROXELS_Y)));
        vec3 lit = vec3_splat(0.0);
        // Without jitter, at 4 points spread over the froxel (a tetrahedron, a quarter froxel out), so shadow edges
        // fade across froxels instead of stepping.
        int taps = u_volFogGrid[9].z > 0.5 ? 4 : 1;
        for (int t = 0; t < 4; ++t) {
            if (t >= taps)
                break;
            vec3 offset = t == 0 ? vec3(0.25, 0.25, 0.25) : t == 1 ? vec3(-0.25, -0.25, 0.25)
                        : t == 2 ? vec3(-0.25, 0.25, -0.25) : vec3(0.25, -0.25, -0.25);
            vec3 q = taps == 1 ? p : kastFroxelPositionAt(cell, offset);
            for (int l = 0; l < MAX_LIGHTS; ++l) {
                if (float(l) >= u_volFogGrid[3].w)
                    break;
                lit += lightAt(l, q, toEye);
            }
        }
        lit /= float(taps);
        light = medium.rgb * (lit * u_volFogGrid[6].w + u_volFogGrid[6].rgb);
    }
    vec4 current = vec4(light, medium.a);
    // The history at p: where the last frame's view saw it (uvw in its froxel grid).
    float weight = u_volFogGrid[7].w;
    if (weight > 0.0) {
        vec4 clip = mul(u_volFogPrev, vec4(p, 1.0));
        vec2 uv = clip.xy / clip.w * vec2(0.5, -0.5) + 0.5;
        float w = kastFroxelSlice(dot(p - u_volFogGrid[7].xyz, u_volFogGrid[8].xyz), u_volFogGrid[0].w) / FROXEL_SLICES;
        if (clip.w > 0.0 && uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0 && w >= 0.0 && w <= 1.0)
            current = mix(current, texture3DLod(s_history, vec3(uv, w), 0.0), weight);
    }
    imageStore(s_scatter, cell, current);
}
