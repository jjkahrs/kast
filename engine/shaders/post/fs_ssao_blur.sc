$input v_texcoord0

#include <bgfx_shader.sh>
#include "ssao.sh"

SAMPLER2D(s_tex0, 0); // fs_ssao_gtao.sc's AO (r visibility, g distance)
SAMPLER2D(s_tex1, 1); // G-data normal + roughness

// Bilateral 4x4 box over the half-res AO (the size of fs_ssao_gtao.sc's dither tile, which it averages out): taps
// weigh by how near their distance is to this pixel's (within 5% + 1 cm) and by their normals' agreement.
void main()
{
    vec2 uv = v_texcoord0;
    vec4 center = texture2DLod(s_tex0, uv, 0.0);
    if (center.y <= 0.0) {
        gl_FragColor = center;
        return;
    }
    vec3 n = kastViewNormal(texture2DLod(s_tex1, uv, 0.0));
    float tolerance = 0.05 * center.y + 0.01;
    float sum = 0.0, weights = 0.0;
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) {
            vec2 tap = uv + vec2(float(x), float(y)) * u_texel.zw;
            vec4 ao = texture2DLod(s_tex0, tap, 0.0);
            vec3 tapN = kastViewNormal(texture2DLod(s_tex1, tap, 0.0));
            float w = saturate(1.0 - abs(ao.y - center.y) / tolerance) * pow(saturate(dot(n, tapN)), 8.0);
            sum += ao.x * w;
            weights += w;
        }
    }
    gl_FragColor = vec4(sum / max(weights, 1e-5), center.y, 0.0, 0.0);
}
