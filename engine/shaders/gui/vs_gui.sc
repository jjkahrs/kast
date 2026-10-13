$input a_position, a_color0, a_texcoord0
$output v_color0, v_texcoord0

#include <bgfx_shader.sh>

// RmlUi geometry: pixel positions, moved by the model transform (translation, CSS transform) into an ortho view.
void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position.xy, 0.0, 1.0));
    v_color0 = a_color0;
    v_texcoord0 = a_texcoord0;
}
