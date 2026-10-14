$input a_position, a_texcoord0, a_indices, a_weight
$output v_texcoord0

#include <bgfx_shader.sh>

#define MAX_BONES 128 // kMaxBones in model.h

uniform mat4 u_bones[MAX_BONES]; // as vs_mesh_skinned.sc

// vs_shadow.sc for skinned meshes: skinned as vs_mesh_skinned.sc, so the shadow follows the pose.
void main()
{
    mat4 skin = a_weight.x * u_bones[int(a_indices.x)] + a_weight.y * u_bones[int(a_indices.y)] +
                a_weight.z * u_bones[int(a_indices.z)] + a_weight.w * u_bones[int(a_indices.w)];
    gl_Position = mul(u_modelViewProj, mul(skin, vec4(a_position, 1.0)));
    v_texcoord0 = a_texcoord0;
}
