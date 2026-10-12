$input a_position, a_normal
$output v_normal

#include <bgfx_shader.sh>

uniform mat4 u_normalMatrix; // transpose(inverse(model)), so non-uniform scale keeps normals perpendicular

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_normal = mul(u_normalMatrix, vec4(a_normal, 0.0)).xyz;
}
