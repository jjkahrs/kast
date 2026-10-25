#include <bgfx_compute.sh>

// Volumetric fog inject (volumetric_fog.h VolumetricFog::update): one thread per froxel. Its medium at its center: the
// scene's height fog (density * exp(-heightFalloff * max(y - baseHeight, 0)), albedo) plus every FogVolume's
// (kastFogVolume: fogVolumeDensity in volumetric_fog.cpp), as rgb scattering (albedo * extinction) and a extinction.
IMAGE3D_WO(s_density, rgba16f, 0);

#define MAX_FOG_VOLUMES 8 // kMaxFogVolumes in volumetric_fog.h

#include "volumetric_fog.sh"

uniform mat4 u_fogVolumeMtx[MAX_FOG_VOLUMES]; // world to its unit shape (-0.5..0.5)
// Per volume: [2i] x density, y edgeFalloff, z heightFalloff, w shape (0 box, 1 sphere); [2i + 1] rgb albedo, w its
// lowest world y.
uniform vec4 u_fogVolumes[MAX_FOG_VOLUMES * 2];

float kastFogVolume(int i, vec3 p)
{
    vec4 params = u_fogVolumes[i * 2];
    vec3 q = mul(u_fogVolumeMtx[i], vec4(p, 1.0)).xyz;
    vec3 a = abs(q);
    float d = 2.0 * (params.w > 0.5 ? length(q) : max(max(a.x, a.y), a.z)); // 0 center, 1 surface
    if (d >= 1.0)
        return 0.0;
    float edge = params.y > 0.0 ? saturate((1.0 - d) / params.y) : 1.0;
    return params.x * edge * exp(-params.z * max(p.y - u_fogVolumes[i * 2 + 1].w, 0.0));
}

NUM_THREADS(4, 4, 4)
void main()
{
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (float(cell.y) >= FROXELS_Y)
        return;
    vec3 p = kastFroxelPosition(cell);
    float extinction = u_volFogGrid[4].x * exp(-u_volFogGrid[4].y * max(p.y - u_volFogGrid[4].z, 0.0));
    vec3 scattering = u_volFogGrid[5].rgb * extinction;
    for (int i = 0; i < MAX_FOG_VOLUMES; ++i) {
        if (float(i) >= u_volFogGrid[2].w)
            break;
        float density = kastFogVolume(i, p);
        extinction += density;
        scattering += u_fogVolumes[i * 2 + 1].rgb * density;
    }
    imageStore(s_density, cell, vec4(scattering, extinction));
}
