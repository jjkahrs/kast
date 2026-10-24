#include <bgfx_compute.sh>

// Hi-Z pyramid (occlusion.h HizOcclusion, buildHizPyramid on the CPU): one pass writes 4 mips. Each 8 x 8 group
// makes an 8 x 8 tile of mip k, then from groupshared memory the 4 x 4, 2 x 2 and 1 x 1 tiles of mips k + 1 .. k + 3,
// each texel the farthest depth under it. Mip 0 (the first pass) takes the camera depth pixels it covers
// (hizDepthSpan: up to 5 per axis, partly covered ones too); later passes the 2 x 2 under it in mip k - 1. Sizes are
// powers of 2, so a tile overhangs only an axis smaller than 8, where clamping repeats texels it already covers.
SAMPLER2D(s_hizDepth, 0);
IMAGE2D_RW(s_hizSource, r32f, 1); // read only; RW, a UAV: an SRV of mip k - 1 read stale cache lines while the
                                   // texture stays bound as a UAV (seen on Direct3D 11)
IMAGE2D_WO(s_hizMip0, r32f, 2);
IMAGE2D_WO(s_hizMip1, r32f, 3);
IMAGE2D_WO(s_hizMip2, r32f, 4);
IMAGE2D_WO(s_hizMip3, r32f, 5);

uniform vec4 u_hiz[2]; // [0] xy: the source's size (the depth target, or mip k - 1), zw: mip k's; [1] x: 1 on the
                       // first pass (from depth), y: mips this pass writes (1..4)

SHARED float s_tile0[64];
SHARED float s_tile1[16];
SHARED float s_tile2[4];

float farthest4(float a, float b, float c, float d)
{
    return max(max(a, b), max(c, d));
}

NUM_THREADS(8, 8, 1)
void main()
{
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 local = ivec2(gl_LocalInvocationID.xy);
    ivec2 group = ivec2(gl_WorkGroupID.xy);
    ivec2 source = ivec2(u_hiz[0].xy);
    ivec2 size = ivec2(u_hiz[0].zw);
    int levels = int(u_hiz[1].y);
    ivec2 at = min(id, size - ivec2(1, 1));
    float farthest = 0.0;
    if (u_hiz[1].x > 0.5) {
        ivec2 first = at * source / size;
        ivec2 last = min(((at + ivec2(1, 1)) * source + size - ivec2(1, 1)) / size - ivec2(1, 1), source - ivec2(1, 1));
        for (int y = first.y; y <= last.y; ++y)
            for (int x = first.x; x <= last.x; ++x)
                farthest = max(farthest, texelFetch(s_hizDepth, ivec2(x, y), 0).x);
    } else {
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                farthest = max(farthest, imageLoad(s_hizSource, min(at * 2 + ivec2(x, y), source - ivec2(1, 1))).x);
    }
    if (id.x < size.x && id.y < size.y)
        imageStore(s_hizMip0, id, vec4(farthest, 0.0, 0.0, 0.0));
    s_tile0[local.y * 8 + local.x] = farthest;
    barrier();

    ivec2 size1 = max(size / 2, ivec2(1, 1));
    if (local.x < 4 && local.y < 4) {
        int i = local.y * 16 + local.x * 2; // (2x, 2y) in the 8-wide tile
        float f = farthest4(s_tile0[i], s_tile0[i + 1], s_tile0[i + 8], s_tile0[i + 9]);
        s_tile1[local.y * 4 + local.x] = f;
        ivec2 texel = group * 4 + local;
        if (levels > 1 && texel.x < size1.x && texel.y < size1.y)
            imageStore(s_hizMip1, texel, vec4(f, 0.0, 0.0, 0.0));
    }
    barrier();

    ivec2 size2 = max(size1 / 2, ivec2(1, 1));
    if (local.x < 2 && local.y < 2) {
        int i = local.y * 8 + local.x * 2; // (2x, 2y) in the 4-wide tile
        float f = farthest4(s_tile1[i], s_tile1[i + 1], s_tile1[i + 4], s_tile1[i + 5]);
        s_tile2[local.y * 2 + local.x] = f;
        ivec2 texel = group * 2 + local;
        if (levels > 2 && texel.x < size2.x && texel.y < size2.y)
            imageStore(s_hizMip2, texel, vec4(f, 0.0, 0.0, 0.0));
    }
    barrier();

    ivec2 size3 = max(size2 / 2, ivec2(1, 1));
    if (local.x == 0 && local.y == 0 && levels > 3 && group.x < size3.x && group.y < size3.y)
        imageStore(s_hizMip3, group, vec4(farthest4(s_tile2[0], s_tile2[1], s_tile2[2], s_tile2[3]), 0.0, 0.0, 0.0));
}
