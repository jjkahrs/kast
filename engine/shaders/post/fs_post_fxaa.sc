$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex0, 0); // final sRGB image
uniform vec4 u_texel; // xy: 1 / size

#define FXAA_REDUCE_MIN (1.0 / 128.0)
#define FXAA_REDUCE_MUL (1.0 / 8.0)
#define FXAA_SPAN_MAX 8.0

// ponytail: FXAA 3.11 "console" variant (Lottes): one blur along the edge direction from 4 diagonal lumas, no
// end-of-edge search; the full quality preset if long near-horizontal edges still stair-step.
void main()
{
    vec2 uv = v_texcoord0;
    vec2 o = u_texel.xy;
    vec3 luma = vec3(0.299, 0.587, 0.114);
    vec4 m = texture2D(s_tex0, uv);
    float lNW = dot(texture2D(s_tex0, uv + vec2(-o.x, -o.y)).rgb, luma);
    float lNE = dot(texture2D(s_tex0, uv + vec2(o.x, -o.y)).rgb, luma);
    float lSW = dot(texture2D(s_tex0, uv + vec2(-o.x, o.y)).rgb, luma);
    float lSE = dot(texture2D(s_tex0, uv + vec2(o.x, o.y)).rgb, luma);
    float lM = dot(m.rgb, luma);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2_splat(-FXAA_SPAN_MAX), vec2_splat(FXAA_SPAN_MAX)) * o;

    vec3 a = 0.5 * (texture2D(s_tex0, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(s_tex0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture2D(s_tex0, uv - dir * 0.5).rgb + texture2D(s_tex0, uv + dir * 0.5).rgb);
    float lB = dot(b, luma);
    gl_FragColor = vec4(lB < lMin || lB > lMax ? a : b, m.a);
}
