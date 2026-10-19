$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

// Shadow map depth pass: position only, plus the texcoord for alpha-mask cutouts (fs_shadow.sc).
void main()
{
    // World then view-projection, as vs_shadow_instanced.sc (see vs_mesh.sc).
    gl_Position = mul(u_viewProj, mul(u_model[0], vec4(a_position, 1.0)));
    v_texcoord0 = a_texcoord0;
}
