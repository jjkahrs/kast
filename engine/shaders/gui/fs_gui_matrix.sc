$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_tex, 0);
uniform mat4 u_matrix; // RmlUi's color matrix (column 3: the constant term, times alpha in premultiplied space)

// The color filters, opacity and drop-shadow's tint: one 4x4 matrix on the premultiplied color, kept premultiplied.
void main()
{
    vec4 color = mul(u_matrix, texture2D(s_tex, v_texcoord0) * v_color0);
    color.a = clamp(color.a, 0.0, 1.0);
    gl_FragColor = vec4(clamp(color.rgb, vec3_splat(0.0), vec3_splat(color.a)), color.a);
}
