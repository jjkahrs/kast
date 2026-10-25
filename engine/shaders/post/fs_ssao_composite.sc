$input v_texcoord0

#include <bgfx_shader.sh>
#include "ssao.sh"

SAMPLER2D(s_tex0, 0); // fs_ssao_blur.sc's AO (half resolution)
SAMPLER2D(s_tex1, 1); // the scene's depth
SAMPLER2D(s_tex2, 2); // G-data normal + roughness
SAMPLER2D(s_tex3, 3); // G-data indirect diffuse

// What AO takes from the opaque scene's color, subtracted by the blend (PostProcess::apply): indirectDiffuse *
// saturate(intensity * (1 - visibility^power)), so direct light and emission are never darkened (aoComposite in
// post_process.cpp). The visibility is upsampled from the 2x2 nearest half-res texels, bilinear weights times how near
// their distance is to this pixel's.
void main()
{
    vec2 uv = v_texcoord0;
    if (texture2DLod(s_tex2, uv, 0.0).a < 0.5) {
        gl_FragColor = vec4_splat(0.0);
        return;
    }
    float dist = kastViewDistance(kastViewPos(uv, texture2DLod(s_tex1, uv, 0.0).x));
    float tolerance = 0.05 * dist + 0.01;
    vec2 texelPos = uv / u_texel.zw - 0.5;
    vec2 base = floor(texelPos);
    vec2 f = texelPos - base;
    float sum = 0.0, weights = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec4 ao = texture2DLod(s_tex0, (base + vec2(float(x), float(y)) + 0.5) * u_texel.zw, 0.0);
            float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            float w = bilinear * max(saturate(1.0 - abs(ao.y - dist) / tolerance), 1e-3);
            sum += ao.x * w;
            weights += w;
        }
    }
    float visibility = saturate(sum / max(weights, 1e-6));
    float occlusion = saturate(u_ssao.z * (1.0 - pow(visibility, u_ssao.w)));
    gl_FragColor = vec4(texture2DLod(s_tex3, uv, 0.0).rgb * occlusion, 0.0);
}
