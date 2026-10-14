$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex0, 0); // the bloom level above, twice this size
uniform vec4 u_texel; // xy: 1 / source size

// Halves the bloom level: 4 bilinear taps over a 4x4 source texel box.
void main()
{
    vec2 o = u_texel.xy;
    gl_FragColor = vec4(0.25 * (texture2D(s_tex0, v_texcoord0 + vec2(-o.x, -o.y)).rgb + texture2D(s_tex0, v_texcoord0 + vec2(o.x, -o.y)).rgb
                              + texture2D(s_tex0, v_texcoord0 + vec2(-o.x, o.y)).rgb + texture2D(s_tex0, v_texcoord0 + vec2(o.x, o.y)).rgb),
                        1.0);
}
