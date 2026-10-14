$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

// Shadow map depth pass: position only, plus the texcoord for alpha-mask cutouts (fs_shadow.sc).
void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_texcoord0 = a_texcoord0;
}
