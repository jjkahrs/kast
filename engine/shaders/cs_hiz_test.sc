#include <bgfx_compute.sh>

// Hi-Z occlusion test (occlusion.h hizOccluded, the same math): one thread per texel of the visibility image, 1 if
// that candidate's box may be visible, 0 if its nearest depth is beyond the farthest depth of the pyramid texels its
// screen rect covers at the mip where that is at most 2 x 2. Texels past the boxes get 1.
SAMPLER2D(s_hizPyramid, 0);
BUFFER_RO(b_hizBoxes, vec4, 1); // per box: min, max (world)
IMAGE2D_WO(s_hizVisible, r32f, 2);

uniform mat4 u_hizViewProj; // the frame whose depth built the pyramid
uniform vec4 u_hiz[2]; // [0] xy: mip 0's size, z: mips, w: boxes; [1] x: homogeneous depth, y: origin bottom-left, z: image width

NUM_THREADS(64, 1, 1)
void main()
{
    int i = int(gl_GlobalInvocationID.x);
    int width = int(u_hiz[1].z);
    ivec2 texel = ivec2(i % width, i / width);
    if (i >= int(u_hiz[0].w)) {
        imageStore(s_hizVisible, texel, vec4(1.0, 0.0, 0.0, 0.0));
        return;
    }
    vec3 lo = b_hizBoxes[i * 2].xyz;
    vec3 hi = b_hizBoxes[i * 2 + 1].xyz;
    vec4 rect = vec4(1e30, 1e30, -1e30, -1e30);
    float nearest = 1e30;
    bool visible = false;
    for (int corner = 0; corner < 8; ++corner) {
        vec3 p = vec3((corner & 1) != 0 ? hi.x : lo.x, (corner & 2) != 0 ? hi.y : lo.y, (corner & 4) != 0 ? hi.z : lo.z);
        vec4 clip = mul(u_hizViewProj, vec4(p, 1.0));
        if (clip.w <= 1e-6) {
            visible = true; // behind the eye
            break;
        }
        vec3 ndc = clip.xyz / clip.w;
        float depth = u_hiz[1].x > 0.5 ? ndc.z * 0.5 + 0.5 : ndc.z;
        if (depth < 0.0) {
            visible = true; // nearer than the near plane
            break;
        }
        vec2 uv = vec2(ndc.x * 0.5 + 0.5, u_hiz[1].y > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5);
        rect = vec4(min(rect.xy, uv), max(rect.zw, uv));
        nearest = min(nearest, depth);
    }
    if (!visible) {
        rect = clamp(rect, 0.0, 1.0);
        vec2 size0 = u_hiz[0].xy;
        int mips = int(u_hiz[0].z);
        vec2 extent = (rect.zw - rect.xy) * size0;
        int mip = clamp(int(ceil(log2(max(max(extent.x, extent.y), 1.0)))), 0, mips - 1);
        ivec4 at = ivec4(0, 0, 0, 0);
        for (int step = 0; step < 2; ++step) {
            ivec2 size = max(ivec2(size0) >> mip, ivec2(1, 1));
            at = clamp(ivec4(rect * vec2(size).xyxy), ivec4(0, 0, 0, 0), (size - ivec2(1, 1)).xyxy);
            if ((at.z - at.x <= 1 && at.w - at.y <= 1) || mip == mips - 1)
                break;
            mip += 1;
        }
        float farthest = texelFetch(s_hizPyramid, at.xy, mip).x;
        farthest = max(farthest, texelFetch(s_hizPyramid, at.zy, mip).x);
        farthest = max(farthest, texelFetch(s_hizPyramid, at.xw, mip).x);
        farthest = max(farthest, texelFetch(s_hizPyramid, at.zw, mip).x);
        visible = nearest <= farthest;
    }
    imageStore(s_hizVisible, texel, vec4(visible ? 1.0 : 0.0, 0.0, 0.0, 0.0));
}
