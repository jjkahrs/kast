$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex0, 0); // this level's downsample
SAMPLER2D(s_tex1, 1); // the level below (half this size), already upsampled
uniform vec4 u_texel; // xy: 1 / size of s_tex1; z: scatter 0..1

// mix(this level, 3x3 tent (1 2 1 / 2 4 2 / 1 2 1, / 16) of the level below, scatter): higher scatter = wider glow.
void main()
{
    vec2 o = u_texel.xy;
    vec2 uv = v_texcoord0;
    vec3 low = 4.0 * texture2D(s_tex1, uv).rgb
             + 2.0 * (texture2D(s_tex1, uv + vec2(o.x, 0.0)).rgb + texture2D(s_tex1, uv - vec2(o.x, 0.0)).rgb
                    + texture2D(s_tex1, uv + vec2(0.0, o.y)).rgb + texture2D(s_tex1, uv - vec2(0.0, o.y)).rgb)
             + texture2D(s_tex1, uv + o).rgb + texture2D(s_tex1, uv - o).rgb
             + texture2D(s_tex1, uv + vec2(o.x, -o.y)).rgb + texture2D(s_tex1, uv + vec2(-o.x, o.y)).rgb;
    gl_FragColor = vec4(mix(texture2D(s_tex0, uv).rgb, low / 16.0, u_texel.z), 1.0);
}
