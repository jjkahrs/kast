$input a_position, a_normal, a_tangent, a_texcoord0, a_color0
$output v_normal, v_tangent, v_worldPos, v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform mat4 u_normalMatrix; // transpose(inverse(model)), so non-uniform scale keeps normals perpendicular

void main()
{
    // World then view-projection, not u_modelViewProj: the same math as vs_mesh_instanced.sc, so a mesh lands on the
    // same pixels drawn either way.
    vec4 worldPos = mul(u_model[0], vec4(a_position, 1.0));
    gl_Position = mul(u_viewProj, worldPos);
    v_normal = mul(u_normalMatrix, vec4(a_normal, 0.0)).xyz;
    // The tangent lies in the surface, so the model matrix (not the normal matrix) carries it; fs_mesh.sc makes it
    // perpendicular to the normal again. A mirroring matrix flips the bitangent: w takes the determinant's sign.
    vec3 c0 = mul(u_model[0], vec4(1.0, 0.0, 0.0, 0.0)).xyz;
    vec3 c1 = mul(u_model[0], vec4(0.0, 1.0, 0.0, 0.0)).xyz;
    vec3 c2 = mul(u_model[0], vec4(0.0, 0.0, 1.0, 0.0)).xyz;
    float mirror = dot(cross(c0, c1), c2) < 0.0 ? -1.0 : 1.0;
    v_tangent = vec4(mul(u_model[0], vec4(a_tangent.xyz, 0.0)).xyz, a_tangent.w * mirror);
    v_worldPos = worldPos.xyz;
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0; // white unless the mesh has vertex colors; only .kshader fragment()s read it (COLOR)
}
