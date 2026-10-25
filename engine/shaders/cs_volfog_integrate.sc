#include <bgfx_compute.sh>

// Volumetric fog integrate (volumetric_fog.h VolumetricFog::update): one thread per froxel column, front to back
// through cs_volfog_scatter.sc's slices (the first from the eye), each slice's light energy-conserving over its
// thickness along the ray (Hillaire 2015: S (1 - e^(-sigma t)) / sigma), times the transmittance in front of it. Writes
// what the eye sees up to each slice's far side into the atlas (slice i at columns i * FROXELS_X ..): rgb in-scattered
// light, a transmittance (kastFroxelFog samples it).
IMAGE3D_RO(s_scatter, rgba16f, 0);
IMAGE2D_WO(s_integrated, rgba16f, 1);

#include "volumetric_fog.sh"

NUM_THREADS(8, 8, 1)
void main()
{
    ivec2 column = ivec2(gl_GlobalInvocationID.xy);
    if (float(column.x) >= FROXELS_X || float(column.y) >= FROXELS_Y)
        return;
    float rayLength = length(kastFroxelRay((vec2(column) + 0.5) / vec2(FROXELS_X, FROXELS_Y))); // per meter of depth
    vec3 light = vec3_splat(0.0);
    float transmittance = 1.0;
    float start = 0.0;
    for (int i = 0; i < int(FROXEL_SLICES); ++i) {
        float end = kastFroxelDepth(float(i + 1), u_volFogGrid[0].w);
        float thickness = (end - start) * rayLength;
        start = end;
        vec4 medium = imageLoad(s_scatter, ivec3(column, i));
        float through = exp(-medium.a * thickness);
        light += transmittance * (medium.rgb - medium.rgb * through) / max(medium.a, 1e-6);
        transmittance *= through;
        imageStore(s_integrated, ivec2(i * int(FROXELS_X) + column.x, column.y), vec4(light, transmittance));
    }
}
