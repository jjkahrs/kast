$input v_texcoord0

#include <bgfx_shader.sh>
#include "dof.sh"

SAMPLER2D(s_tex0, 0); // the scene's HDR color (after the blended draws)
SAMPLER2D(s_tex1, 1); // the scene's depth

// Half resolution: rgb the 2x2 full-res texels' color (one bilinear tap), a their signed CoC in half-res pixels: the
// nearest's if any is in the near field (so the near blur reaches past its silhouette), else their mean.
void main()
{
    vec2 uv = v_texcoord0;
    float nearest = 0.0, sum = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 tap = uv + (vec2(float(x), float(y)) - 0.5) * u_texel.xy;
            float coc = kastCoc(kastViewPos(tap, texture2DLod(s_tex1, tap, 0.0).x).z);
            nearest = min(nearest, coc);
            sum += coc;
        }
    }
    gl_FragColor = vec4(texture2DLod(s_tex0, uv, 0.0).rgb, 0.5 * (nearest < 0.0 ? nearest : 0.25 * sum));
}
