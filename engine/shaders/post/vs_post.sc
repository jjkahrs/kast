$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

// Full-screen triangle in clip space (post_process.cpp), no transform.
void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
}
