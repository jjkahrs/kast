$input a_position, a_normal, a_tangent, a_texcoord0, a_color0, i_data0, i_data1, i_data2, i_data3
$output v_normal, v_tangent, v_worldPos, v_texcoord0, v_color0

#include <bgfx_shader.sh>
#include <mesh.sh>

// vs_mesh.sc's template with the model matrix per instance (i_data0..3: its rows as bx stores them) instead of u_model.
//@user
#ifndef KAST_VERTEX
void vertex(inout KastVertex kast_v) {}
#endif

void main()
{
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    KastVertex v = kastVertex(a_position, a_normal, a_texcoord0, a_color0, model, int(gl_InstanceID));
    vertex(v);
    vec4 worldPos = mul(model, vec4(v.vertex, 1.0));
    gl_Position = mul(u_viewProj, worldPos);
    // transpose(inverse(model)) applied to the normal, from the cofactors: inverse's rows are cross products of the
    // basis columns over the determinant.
    vec3 c0 = i_data0.xyz;
    vec3 c1 = i_data1.xyz;
    vec3 c2 = i_data2.xyz;
    vec3 r0 = cross(c1, c2);
    float det = dot(c0, r0);
    v_normal = (r0 * v.normal.x + cross(c2, c0) * v.normal.y + cross(c0, c1) * v.normal.z) / det;
    v_tangent = vec4(mul(model, vec4(a_tangent.xyz, 0.0)).xyz, a_tangent.w * (det < 0.0 ? -1.0 : 1.0)); // as vs_mesh.sc
    v_worldPos = worldPos.xyz;
    v_texcoord0 = v.uv;
    v_color0 = v.color; // white unless the mesh has vertex colors; only .kshaders read it (COLOR)
}
