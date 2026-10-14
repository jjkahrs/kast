$input a_position, a_normal, a_texcoord0, a_indices, a_weight
$output v_normal, v_worldPos, v_texcoord0

#include <bgfx_shader.sh>

#define MAX_BONES 128 // kMaxBones in model.h

uniform mat4 u_normalMatrix;     // transpose(inverse(model)), as vs_mesh.sc
uniform mat4 u_bones[MAX_BONES]; // mesh space to model space per joint (inverse bind, then the posed joint)

void main()
{
    // Up to 4 joints per vertex; the weights sum to 1.
    mat4 skin = a_weight.x * u_bones[int(a_indices.x)] + a_weight.y * u_bones[int(a_indices.y)] +
                a_weight.z * u_bones[int(a_indices.z)] + a_weight.w * u_bones[int(a_indices.w)];
    vec4 position = mul(skin, vec4(a_position, 1.0));
    gl_Position = mul(u_modelViewProj, position);
    // ponytail: skin rotates normals as-is; joints with non-uniform scale would need its inverse transpose.
    v_normal = mul(u_normalMatrix, mul(skin, vec4(a_normal, 0.0))).xyz;
    v_worldPos = mul(u_model[0], position).xyz;
    v_texcoord0 = a_texcoord0;
}
