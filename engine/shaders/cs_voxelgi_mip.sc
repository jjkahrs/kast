#include <bgfx_compute.sh>

// VoxelGI mips (voxel_gi.h VoxelGiRenderer::update): one thread per voxel of level i + 1 of a radiance volume, the mean
// of the 2 x 2 x 2 voxels under it in level i (rgb premultiplied by opacity, so empty voxels thin the light and the
// occlusion alike). Odd sizes repeat their last voxel.
IMAGE3D_RO(s_source, rgba16f, 0);
IMAGE3D_WO(s_target, rgba16f, 1);

uniform vec4 u_voxel[4]; // [0] xyz: level i's size (voxels), [1] xyz: level i + 1's

NUM_THREADS(4, 4, 4)
void main()
{
    ivec3 id = ivec3(gl_GlobalInvocationID);
    ivec3 size = ivec3(u_voxel[1].xyz);
    if (id.x >= size.x || id.y >= size.y || id.z >= size.z)
        return;
    ivec3 last = ivec3(u_voxel[0].xyz) - ivec3(1, 1, 1);
    vec4 sum = vec4_splat(0.0);
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                sum += imageLoad(s_source, min(id * 2 + ivec3(x, y, z), last));
    imageStore(s_target, id, sum * 0.125);
}
