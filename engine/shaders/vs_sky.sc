$input a_position
$output v_worldPos

#include <bgfx_shader.sh>

uniform mat4 u_skyInvViewProj; // inverse(view rotation * projection): clip space to world space, eye at the origin

// Full-screen triangle at the far plane (sky.cpp). v_worldPos: the world-space view direction through the vertex.
void main()
{
    gl_Position = vec4(a_position.xy, 1.0, 1.0);
    vec4 far = mul(u_skyInvViewProj, vec4(a_position.xy, 1.0, 1.0));
    v_worldPos = far.xyz / far.w;
}
