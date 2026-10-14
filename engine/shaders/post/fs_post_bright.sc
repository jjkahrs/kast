$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex0, 0); // HDR scene, full size
uniform vec4 u_texel; // xy: 1 / scene size
uniform vec4 u_bloom; // x: threshold, y: knee

// Bloom bright pass at half size: a 4x4 texel box (4 bilinear taps), then a soft threshold on its luminance
// (Unity / Karis style quadratic knee): w = max(clamp(l - t + k, 0, 2k)^2 / 4k, l - t) / l.
void main()
{
    vec2 o = u_texel.xy;
    vec3 c = 0.25 * (texture2D(s_tex0, v_texcoord0 + vec2(-o.x, -o.y)).rgb + texture2D(s_tex0, v_texcoord0 + vec2(o.x, -o.y)).rgb
                   + texture2D(s_tex0, v_texcoord0 + vec2(-o.x, o.y)).rgb + texture2D(s_tex0, v_texcoord0 + vec2(o.x, o.y)).rgb);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float t = u_bloom.x;
    float k = u_bloom.y;
    float soft = clamp(l - t + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k + 1e-4);
    gl_FragColor = vec4(c * (max(soft, l - t) / max(l, 1e-4)), 1.0);
}
