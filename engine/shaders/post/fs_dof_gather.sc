$input v_texcoord0

#include <bgfx_shader.sh>
#include "dof.sh"

SAMPLER2D(s_tex0, 0); // fs_dof_setup.sc's color + CoC (half resolution)

#define KAST_DOF_TAPS 64
#define KAST_GOLDEN_ANGLE 2.39996323

// Disc bokeh as scatter-as-gather over a uniform (Vogel) disc of taps, at half resolution, in two layers:
// far (gl_FragData[0]): taps over this pixel's own far CoC, each kept where its own CoC reaches this pixel, so a sharp
// foreground never smears into the blurred background; equal weights make bright highlights flat discs.
// near (gl_FragData[1]): taps over max CoC, kept where their near CoC reaches this pixel, each weighed by 1 / its
// disc's area (what one tap's color spreads over); a is the coverage, sum over kept taps of (maxCoc / coc)^2 / taps
// (1 inside a uniform near object, falling off smoothly past its edge: the near blur bleeds over in-focus pixels
// without a hard halo).
void main()
{
    vec2 uv = v_texcoord0;
    vec4 center = texture2DLod(s_tex0, uv, 0.0);
    float farRadius = max(center.a, 0.0);
    float nearRadius = 0.5 * u_dof.z;
    vec3 far = center.rgb, near = vec3_splat(0.0);
    float farWeight = 1.0, nearWeight = 0.0, coverage = 0.0;
    for (int i = 0; i < KAST_DOF_TAPS; ++i) {
        float a = float(i) * KAST_GOLDEN_ANGLE;
        vec2 dir = vec2(cos(a), sin(a)) * sqrt((float(i) + 0.5) / float(KAST_DOF_TAPS));
        if (farRadius > 0.5) {
            vec2 offset = dir * farRadius;
            vec4 s = texture2DLod(s_tex0, uv + offset * u_texel.zw, 0.0);
            float w = saturate(s.a - length(offset) + 0.5);
            far += s.rgb * w;
            farWeight += w;
        }
        vec2 offset = dir * nearRadius;
        vec4 s = texture2DLod(s_tex0, uv + offset * u_texel.zw, 0.0);
        float coc = max(-s.a, 0.5);
        float w = saturate(-s.a - 0.5) * saturate(coc - length(offset) + 0.5);
        float area = 1.0 / (coc * coc);
        near += s.rgb * w * area;
        nearWeight += w * area;
        coverage += w * area;
    }
    gl_FragData[0] = vec4(far / farWeight, 1.0);
    gl_FragData[1] = vec4(near / max(nearWeight, 1e-6),
                          saturate(coverage * nearRadius * nearRadius / float(KAST_DOF_TAPS)));
}
