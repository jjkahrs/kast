// Shared by the mesh templates (fs_mesh.sc and vs_mesh*.sc; shader.cpp generateShader puts a .kshader at their
// "//@user" line): the material and frame uniforms, the built-ins every stage has and, in vertex shaders, what vertex()
// reads and writes. Every name #defined here is reserved (shaderBuiltinNames in shader.cpp).

#define PI 3.14159265

uniform vec4 u_baseColor;   // linear RGBA, times the base color texture
uniform vec4 u_emissive;    // rgb: linear, added after lighting
uniform vec4 u_material;    // x: metallic, y: roughness, z: alpha cutoff (-1 = none), w: 0 opaque, 1 blend, 2 additive
uniform vec4 u_uvTransform; // xy: scale, zw: offset; every texture of the material samples uv * scale + offset
uniform vec4 u_time;        // x: seconds, wrapping every kShaderTimePeriod (shader_clock.h)
uniform vec4 u_cameraPos;   // xyz: world position of the eye

#define TIME (u_time.x)
#define CAMERA_POSITION (u_cameraPos.xyz)
#define VIEW_MATRIX (u_view)       // in shadow passes: the light's
#define PROJECTION_MATRIX (u_proj) // in shadow passes: the light's
#define MATERIAL_BASE_COLOR (u_baseColor)
#define MATERIAL_METALLIC (u_material.x)
#define MATERIAL_ROUGHNESS (u_material.y)
#define MATERIAL_EMISSION (u_emissive.rgb)
#define MATERIAL_ALPHA_CUTOFF (u_material.z)
#define MATERIAL_UV_TRANSFORM (u_uvTransform)

#if BGFX_SHADER_TYPE_VERTEX
// What vertex() reads and writes (the uppercase built-ins below). main() fills it with the mesh's vertex, so a vertex()
// that writes nothing changes nothing; then transforms what it wrote to the world.
struct KastVertex {
    vec3 vertex;    // object space; skinned meshes: after skinning
    vec3 normal;    // object space, as the mesh stores it (not unit after skinning)
    vec2 uv;        // UV0 as the mesh stores it; fragment()'s UV is this * uvScale + uvOffset
    vec4 color;     // vertex color (COLOR0), white if the mesh has none
    mat4 model;     // read only: object to world (instanced: the instance's)
    int instanceId; // read only: the index in its instanced draw, 0 when not instanced
};

#define VERTEX kast_v.vertex
#define NORMAL kast_v.normal
#define UV kast_v.uv
#define COLOR kast_v.color
#define MODEL_MATRIX kast_v.model
#define WORLD_MATRIX kast_v.model
#define INSTANCE_ID kast_v.instanceId

KastVertex kastVertex(vec3 position, vec3 normal, vec2 uv, vec4 color, mat4 model, int instanceId)
{
    KastVertex v;
    v.vertex = position;
    v.normal = normal;
    v.uv = uv;
    v.color = color;
    v.model = model;
    v.instanceId = instanceId;
    return v;
}
#endif
