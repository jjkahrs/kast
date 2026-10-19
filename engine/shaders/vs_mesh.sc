$input a_position, a_normal, a_texcoord0
$output v_normal, v_worldPos, v_texcoord0

#include <bgfx_shader.sh>

uniform mat4 u_normalMatrix; // transpose(inverse(model)), so non-uniform scale keeps normals perpendicular

void main()
{
    // World then view-projection, not u_modelViewProj: the same math as vs_mesh_instanced.sc, so a mesh lands on the
    // same pixels drawn either way.
    vec4 worldPos = mul(u_model[0], vec4(a_position, 1.0));
    gl_Position = mul(u_viewProj, worldPos);
    v_normal = mul(u_normalMatrix, vec4(a_normal, 0.0)).xyz;
    v_worldPos = worldPos.xyz;
    v_texcoord0 = a_texcoord0;
}
