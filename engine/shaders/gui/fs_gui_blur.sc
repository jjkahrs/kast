$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);
uniform vec4 u_blur;       // xy: one texel along the blur direction, in uv
uniform vec4 u_weights[2]; // Gaussian weights for offsets 0..6 texels (they sum to 1 over -6..6)

#define TAPS(_d, _w) ((texture2D(s_tex, v_texcoord0 + (_d) * u_blur.xy) + texture2D(s_tex, v_texcoord0 - (_d) * u_blur.xy)) * (_w))

// One direction of a separable 13-tap Gaussian blur on premultiplied color.
void main()
{
    gl_FragColor = texture2D(s_tex, v_texcoord0) * u_weights[0].x + TAPS(1.0, u_weights[0].y) + TAPS(2.0, u_weights[0].z)
                 + TAPS(3.0, u_weights[0].w) + TAPS(4.0, u_weights[1].x) + TAPS(5.0, u_weights[1].y)
                 + TAPS(6.0, u_weights[1].z);
}
