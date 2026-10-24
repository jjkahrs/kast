$input a_position, a_normal, a_tangent, a_texcoord0, a_color0, a_indices, a_weight
$output v_normal, v_tangent, v_worldPos, v_texcoord0, v_color0, v_lightmapUv

#include <bgfx_shader.sh>
#include <mesh.sh>

#define MAX_BONES 128 // kMaxBones in model.h

uniform mat4 u_normalMatrix;     // transpose(inverse(model)), as vs_mesh.sc
uniform mat4 u_bones[MAX_BONES]; // mesh space to model space per joint (inverse bind, then the posed joint)

// vs_mesh.sc's template for skinned meshes: vertex() gets the skinned vertex, in object space.
//@user
#ifndef KAST_VERTEX
void vertex(inout KastVertex kast_v) {}
#endif

void main()
{
    // Up to 4 joints per vertex; the weights sum to 1.
    mat4 skin = a_weight.x * u_bones[int(a_indices.x)] + a_weight.y * u_bones[int(a_indices.y)] +
                a_weight.z * u_bones[int(a_indices.z)] + a_weight.w * u_bones[int(a_indices.w)];
    vec4 position = mul(skin, vec4(a_position, 1.0));
    // ponytail: skin rotates normals as-is; joints with non-uniform scale would need its inverse transpose.
    KastVertex v = kastVertex(position.xyz, mul(skin, vec4(a_normal, 0.0)).xyz, a_texcoord0, a_color0, u_model[0], 0);
    vertex(v);
    gl_Position = mul(u_modelViewProj, vec4(v.vertex, position.w)); // w: the weights' sum, as before vertex()
    v_normal = mul(u_normalMatrix, vec4(v.normal, 0.0)).xyz;
    // The tangent is skinned like the normal, then carried by the model matrix as in vs_mesh.sc (mirroring included).
    vec3 c0 = mul(u_model[0], vec4(1.0, 0.0, 0.0, 0.0)).xyz;
    vec3 c1 = mul(u_model[0], vec4(0.0, 1.0, 0.0, 0.0)).xyz;
    vec3 c2 = mul(u_model[0], vec4(0.0, 0.0, 1.0, 0.0)).xyz;
    float mirror = dot(cross(c0, c1), c2) < 0.0 ? -1.0 : 1.0;
    v_tangent = vec4(mul(u_model[0], mul(skin, vec4(a_tangent.xyz, 0.0))).xyz, a_tangent.w * mirror);
    v_worldPos = mul(u_model[0], vec4(v.vertex, position.w)).xyz;
    v_texcoord0 = v.uv;
    v_color0 = v.color; // white unless the mesh has vertex colors; only .kshaders read it (COLOR)
    v_lightmapUv = vec2_splat(0.0); // skinned meshes are never lightmapped
}
