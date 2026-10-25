$input v_texcoord0

#include <bgfx_shader.sh>
#include "screen_space.sh"

SAMPLER2D(s_tex0, 0); // fs_ssr_trace.sc's reflections (half resolution: rgb color * confidence, a confidence)
SAMPLER2D(s_tex1, 1); // the scene's depth
SAMPLER2D(s_tex2, 2); // G-data normal + roughness
SAMPLER2D(s_tex3, 3); // G-data indirect specular
SAMPLER2D(s_tex4, 4); // G-data specular weight

uniform vec4 u_ssr; // x: intensity; y: max roughness

// What SSR changes in the opaque scene's color, added by the blend (PostProcess::apply): where rays hit, the indirect
// specular is replaced by the reflected color times the surface's specular weight, by confidence (ssrComposite in
// post_process.cpp): intensity * confidence * (weight * reflection - indirectSpecular); a miss (confidence 0) leaves the
// probe / IBL specular already in the color. The reflection is a bilateral 4x4 gather over the half-res trace, spread
// with roughness (averaging the trace's GGX samples into a blurrier reflection; each tap on its own phase of the trace's
// dither tile), weighed by how near the taps' distance is to this pixel's (within 5% + 1 cm), their normals' agreement
// and 1 / (1 + luma).
void main()
{
    vec2 uv = v_texcoord0;
    vec4 g = texture2DLod(s_tex2, uv, 0.0);
    if (g.a < 0.5 || g.z >= u_ssr.y) {
        gl_FragColor = vec4_splat(0.0);
        return;
    }
    float dist = length(kastViewPos(uv, texture2DLod(s_tex1, uv, 0.0).x));
    float tolerance = 0.05 * dist + 0.01;
    vec3 n = kastViewNormal(g);
    float spread = 1.0 + 6.0 * g.z; // half-res pixels between taps
    vec2 base = floor(uv / u_texel.zw); // this pixel's half-res texel
    vec3 sum = vec3_splat(0.0);
    float confidence = 0.0, weights = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            // Each tap snapped to the nearest half-res texel on its own phase of the trace's 4x4 dither tile (base + i
            // - 2 mod 4), so the 16 taps average all 16 GGX / jitter samples at any spread: a spread landing every tap
            // on the same phase (e.g. 4) left the tile's lattice as a dot grid.
            vec2 i = vec2(float(x), float(y)) - 2.0;
            vec2 texel = base + i + 4.0 * floor(((i + 0.5) * spread - i) / 4.0 + 0.5);
            vec2 tap = (texel + 0.5) * u_texel.zw;
            vec4 ssr = texture2DLod(s_tex0, tap, 0.0);
            vec2 full = (floor(tap / u_texel.xy) + 0.5) * u_texel.xy; // the full-res texel the trace read there
            float tapDist = length(kastViewPos(full, texture2DLod(s_tex1, full, 0.0).x));
            vec3 tapN = kastViewNormal(texture2DLod(s_tex2, full, 0.0));
            float w = saturate(1.0 - abs(tapDist - dist) / tolerance) * pow(saturate(dot(n, tapN)), 8.0);
            // Karis's 1 / (1 + luma) weight: a lone hit on a bright highlight among misses stays faint, not a firefly.
            w /= 1.0 + dot(ssr.rgb / max(ssr.a, 1e-5), vec3(0.2126, 0.7152, 0.0722));
            sum += ssr.rgb * w;
            confidence += ssr.a * w;
            weights += w;
        }
    }
    vec3 weight = texture2DLod(s_tex4, uv, 0.0).rgb;
    vec3 specular = texture2DLod(s_tex3, uv, 0.0).rgb;
    gl_FragColor = vec4(u_ssr.x * (weight * sum - specular * confidence) / max(weights, 1e-5), 0.0);
}
