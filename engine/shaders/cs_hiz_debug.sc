#include <bgfx_compute.sh>

// Hi-Z debug view (occlusion.h HizOcclusion::setDebugMip): one mip of the pyramid as gray, brighter = nearer (about
// sqrt of 10 x (1 - depth): white within a few metres, black at the far plane).
SAMPLER2D(s_hizPyramid, 0);
IMAGE2D_WO(s_hizPicture, rgba8, 1);

uniform vec4 u_hiz[2]; // [0] x: mip, yz: its size

NUM_THREADS(8, 8, 1)
void main()
{
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (id.x >= int(u_hiz[0].y) || id.y >= int(u_hiz[0].z))
        return;
    float depth = texelFetch(s_hizPyramid, id, int(u_hiz[0].x)).x;
    float gray = sqrt(clamp((1.0 - depth) * 10.0, 0.0, 1.0));
    imageStore(s_hizPicture, id, vec4(gray, gray, gray, 1.0));
}
