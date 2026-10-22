$input v_normal, v_tangent, v_worldPos, v_texcoord0, v_color0

// The mesh fragment template: Kast's built-in shader as is, and every .kshader's fragment() at the //@user line
// (shader.cpp generateShader; see docs/engine/shaders.html).

#include <bgfx_shader.sh>

#define MAX_POINT_LIGHTS 8 // kMaxPointLights in renderer.h
#define MAX_CASCADES 4     // kMaxShadowCascades in scene.h
#define CSM_TEXEL (1.0 / 4096.0)        // one texel of the cascade atlas: 2x2 kCascadeSize in shadow.h
#define POINT_SHADOW_SIZE 1024.0        // kPointShadowSize in shadow.h
#define POINT_SHADOW_NEAR 0.05          // kPointShadowNear in shadow.h
#define PI 3.14159265

SAMPLER2D(s_baseColor, 0);         // sRGB, sampled as linear; white if the material has none
SAMPLER2D(s_metallicRoughness, 1); // linear, glTF packing: G = roughness, B = metallic; white if none
SAMPLER2DSHADOW(s_csm, 2);         // directional light cascades, a 2x2 atlas (cascade i at column i % 2, row i / 2)
SAMPLERCUBESHADOW(s_pointShadow0, 3); // shadowed point light cubes: depth of each face's 90 degree projection
SAMPLERCUBESHADOW(s_pointShadow1, 4);
SAMPLERCUBESHADOW(s_pointShadow2, 5);
SAMPLERCUBESHADOW(s_pointShadow3, 6);
SAMPLER2D(s_normal, 7);            // linear tangent-space normal, glTF / OpenGL (+Y green); flat (0.5, 0.5, 1) if none
SAMPLER2D(s_occlusion, 8);         // linear, R = ambient occlusion; white if none
SAMPLER2D(s_emissive, 9);          // sRGB, sampled as linear, times u_emissive; white if none

uniform vec4 u_baseColor;  // linear RGBA, times the base color texture
uniform vec4 u_emissive;   // rgb: linear, added after lighting
uniform vec4 u_material;   // x: metallic, y: roughness, z: alpha cutoff (-1 = none), w: 0 opaque, 1 blend, 2 additive
uniform vec4 u_materialExt; // x: normal scale, y: occlusion strength, z: 1 if a normal texture is bound, w: 1 if unlit
uniform vec4 u_uvTransform; // xy: scale, zw: offset; every texture of the material samples uv * scale + offset
uniform vec4 u_time;        // x: seconds, wrapping every kShaderTimePeriod (shader_clock.h)
uniform vec4 u_cameraPos;  // xyz: world position of the eye
uniform vec4 u_lightDir;   // xyz: world direction the light travels; w: 1 if the scene has a light, else 0
uniform vec4 u_lightColor; // rgb: light color * intensity
uniform vec4 u_ambient;    // rgb: scene ambient color * intensity
uniform vec4 u_pointPosRange[MAX_POINT_LIGHTS]; // xyz: world position; w: range (unused slots: 1)
uniform vec4 u_pointColor[MAX_POINT_LIGHTS];    // rgb: color * intensity (unused slots: 0)
uniform vec4 u_fogColor;  // rgb: linear fog color; w: 1 if fog is on
uniform vec4 u_fogParams; // x: density, y: start distance
uniform mat4 u_csmMtx[MAX_CASCADES];  // world to (atlas u, v, depth) per cascade
uniform vec4 u_csmSplits;             // far view depth of each cascade (unused cascades: the last's)
uniform vec4 u_csmBias[MAX_CASCADES]; // x: normal offset (meters), y: depth bias, per cascade
uniform vec4 u_shadowParams;          // x: 1 if this mesh receives shadows, y: 1 if the directional light casts them
uniform vec4 u_pointShadow[MAX_POINT_LIGHTS]; // per point light slot: x: cube 0..3 (-1: unshadowed), y: bias, z: range

// Windowed inverse square, as pointLightFalloff in renderer.cpp: saturate(1 - (d/r)^4)^2 / (d^2 + 1).
float falloff(float d, float range)
{
    float ratio = d / range;
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window / (d * d + 1.0);
}

// 3x3 PCF of hardware-compared (bilinear) taps: 1 lit, 0 in shadow.
float csmShadow(vec3 worldPos, vec3 n)
{
    float depth = -mul(u_view, vec4(worldPos, 1.0)).z; // right-handed view space looks down -Z
    if (u_shadowParams.y < 0.5 || depth > u_csmSplits.w) // .w: the last cascade's far end, the shadow distance
        return 1.0;
    int c = int(depth > u_csmSplits.x) + int(depth > u_csmSplits.y) + int(depth > u_csmSplits.z);
    vec3 coord = mul(u_csmMtx[c], vec4(worldPos + n * u_csmBias[c].x, 1.0)).xyz;
    coord.z -= u_csmBias[c].y;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += shadow2D(s_csm, vec3(coord.xy + vec2(x, y) * CSM_TEXEL, coord.z));
    return lit / 9.0;
}

float cubeCompare(int cube, vec4 coord)
{
    if (cube == 0)
        return shadowCube(s_pointShadow0, coord);
    if (cube == 1)
        return shadowCube(s_pointShadow1, coord);
    if (cube == 2)
        return shadowCube(s_pointShadow2, coord);
    return shadowCube(s_pointShadow3, coord);
}

// toFragment: from the light to the fragment. Each face stores the depth of a 90 degree left-handed perspective
// projection from POINT_SHADOW_NEAR to range (D3D 0..1 depth), so the reference depth comes from the major axis.
// 3x3 PCF across the face, one texel apart.
float pointShadow(vec4 shadow, vec3 toFragment, vec3 n)
{
    float texel = 2.0 * length(toFragment) / POINT_SHADOW_SIZE; // meters per texel at that distance (roughly)
    vec3 d = toFragment + n * texel; // the normal offset: against acne on surfaces facing away from the light
    vec3 a = abs(d);
    float m = max(max(a.x, a.y), a.z) - shadow.y;
    float range = shadow.z;
    float z = range / (range - POINT_SHADOW_NEAR) - range * POINT_SHADOW_NEAR / ((range - POINT_SHADOW_NEAR) * max(m, 1e-4));
    vec3 dir = normalize(d);
    vec3 t = normalize(cross(abs(dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), dir));
    vec3 b = cross(dir, t);
    int cube = int(shadow.x);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += cubeCompare(cube, vec4(d + (t * float(x) + b * float(y)) * texel, z));
    return lit / 9.0;
}

// Cook-Torrance microfacet BRDF, alpha = roughness^2 (glTF / UE4):
//   D (GGX / Trowbridge-Reitz)  = alpha^2 / (PI * ((N.H)^2 * (alpha^2 - 1) + 1)^2)
//   G (Smith, Schlick-GGX)      = G1(N.V) * G1(N.L), G1(x) = x / (x * (1 - k) + k), k = (roughness + 1)^2 / 8
//   F (Schlick)                 = F0 + (1 - F0) * (1 - V.H)^5, F0 = mix(0.04, baseColor, metallic)
//   specular = D * G * F / (4 * N.V * N.L), diffuse = (1 - F) * (1 - metallic) * baseColor / PI
// Light colors are taken as radiance * PI (the usual real-time convention), so a white light on a rough white
// dielectric gives baseColor * N.L, as the former Lambert shading did: out = (kd * baseColor + PI * specular) * light * N.L.
vec3 shade(vec3 n, vec3 v, vec3 l, vec3 light, vec3 albedo, vec3 f0, float metallic, float roughness)
{
    float nl = max(dot(n, l), 0.0);
    if (nl <= 0.0)
        return vec3_splat(0.0);
    vec3 h = normalize(v + l);
    float nv = max(dot(n, v), 1e-4);
    float nh = max(dot(n, h), 0.0);
    float vh = max(dot(v, h), 0.0);
    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    float denom = nh * nh * (a2 - 1.0) + 1.0;
    float d = a2 / (PI * denom * denom);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float g = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
    vec3 f = f0 + (vec3_splat(1.0) - f0) * pow(saturate(1.0 - vh), 5.0);
    vec3 specular = d * g * f / (4.0 * nv * nl);
    vec3 kd = (vec3_splat(1.0) - f) * (1.0 - metallic);
    return (kd * albedo + PI * specular) * light * nl;
}

// Fog, as fogFactor in post_process.cpp: 1 - exp(-(density * max(d - start, 0))^2). Alpha-blended surfaces fade to the
// fog color; additive ones (u_material.w 2) fade toward black, as additive particles do (adding fog would brighten).
vec3 fogged(vec3 color, vec3 worldPos)
{
    if (u_fogColor.w < 0.5)
        return color;
    float fd = u_fogParams.x * max(length(u_cameraPos.xyz - worldPos) - u_fogParams.y, 0.0);
    float fog = 1.0 - exp(-fd * fd);
    return u_material.w > 1.5 ? color * (1.0 - fog) : mix(color, u_fogColor.rgb, fog);
}

// What fragment() reads and writes (the uppercase built-ins below). main() fills it with what the built-in material
// computes, so a fragment() that writes nothing changes nothing.
struct KastFragment {
    vec2 uv;             // UV0 * uvScale + uvOffset: where the material's textures sample
    vec4 color;          // vertex color (COLOR0), white if the mesh has none
    vec3 normal;         // world, unit; the normal map then bends it
    vec4 tangent;        // world xyz (interpolated), w: bitangent handedness
    vec3 worldPos;
    vec3 viewPos;        // right-handed view space: the camera looks down -Z
    vec4 fragCoord;      // window pixel position, as gl_FragCoord
    vec3 albedo;         // linear base color
    float alpha;
    float metallic;
    float roughness;
    vec3 emission;       // linear HDR, added after lighting
    vec3 normalMap;      // tangent-space normal as stored in a texture (0..1, flat = 0.5, 0.5, 1)
    float normalMapDepth; // scales the normal map's XY (the material's normalScale)
    float ao;            // ambient occlusion: darkens ambient light only
};

// Built-ins: fragment() inputs and outputs (fields of its kast_f), uniforms and texture helpers. The names are reserved:
// a .kshader param may not take one (kBuiltinNames in shader.cpp lists them).
#define UV kast_f.uv
#define COLOR kast_f.color
#define NORMAL kast_f.normal
#define TANGENT kast_f.tangent
#define WORLD_POSITION kast_f.worldPos
#define VIEW_POSITION kast_f.viewPos
#define FRAG_COORD kast_f.fragCoord
#define ALBEDO kast_f.albedo
#define ALPHA kast_f.alpha
#define METALLIC kast_f.metallic
#define ROUGHNESS kast_f.roughness
#define EMISSION kast_f.emission
#define NORMAL_MAP kast_f.normalMap
#define NORMAL_MAP_DEPTH kast_f.normalMapDepth
#define AO kast_f.ao
#define TIME (u_time.x)
#define CAMERA_POSITION (u_cameraPos.xyz)
#define MATERIAL_BASE_COLOR (u_baseColor)
#define MATERIAL_METALLIC (u_material.x)
#define MATERIAL_ROUGHNESS (u_material.y)
#define MATERIAL_EMISSION (u_emissive.rgb)
#define MATERIAL_ALPHA_CUTOFF (u_material.z)
#define MATERIAL_UV_TRANSFORM (u_uvTransform)
vec4 sampleBaseColor(vec2 uv) { return texture2D(s_baseColor, uv); }
vec4 sampleMetallicRoughness(vec2 uv) { return texture2D(s_metallicRoughness, uv); }
vec4 sampleNormal(vec2 uv) { return texture2D(s_normal, uv); }
vec4 sampleOcclusion(vec2 uv) { return texture2D(s_occlusion, uv); }
vec4 sampleEmissive(vec2 uv) { return texture2D(s_emissive, uv); }

//@user
#ifndef KAST_FRAGMENT
void fragment(inout KastFragment kast_f) {}
#endif

void main()
{
    KastFragment f;
    f.uv = v_texcoord0 * u_uvTransform.xy + u_uvTransform.zw;
    vec4 base = u_baseColor * texture2D(s_baseColor, f.uv);
    vec4 mr = texture2D(s_metallicRoughness, f.uv);
    f.color = v_color0;
    f.normal = normalize(v_normal);
    f.tangent = v_tangent;
    f.worldPos = v_worldPos;
    f.viewPos = mul(u_view, vec4(v_worldPos, 1.0)).xyz;
    f.fragCoord = gl_FragCoord;
    f.albedo = base.rgb;
    f.alpha = base.a;
    f.metallic = u_material.x * mr.b;
    f.roughness = u_material.y * mr.g;
    f.emission = u_emissive.rgb * texture2D(s_emissive, f.uv).rgb;
    f.normalMap = texture2D(s_normal, f.uv).xyz;
    f.normalMapDepth = u_materialExt.x; // glTF normalTexture.scale
    // Ambient occlusion darkens ambient light only: ao = 1 + strength * (R - 1).
    f.ao = 1.0 + u_materialExt.y * (texture2D(s_occlusion, f.uv).r - 1.0);
    fragment(f);

    if (f.alpha < u_material.z)
        discard;
    float alpha = u_material.w > 0.5 ? f.alpha : 1.0;
    if (u_materialExt.w > 0.5) { // unlit
        gl_FragColor = vec4(fogged(f.albedo + f.emission, v_worldPos), alpha);
        return;
    }
    float metallic = saturate(f.metallic);
    float roughness = clamp(f.roughness, 0.04, 1.0); // a 0 roughness highlight is infinitely thin: floored
    vec3 albedo = f.albedo;
    vec3 f0 = mix(vec3_splat(0.04), albedo, metallic);

    vec3 n = f.normal;
    // A shader writing NORMAL or NORMAL_MAP (KAST_CUSTOM_NORMAL) always gets this, which also renormalizes NORMAL.
#ifdef KAST_CUSTOM_NORMAL
    if (true) {
#else
    if (u_materialExt.z > 0.5) {
#endif
        // Gram-Schmidt: the interpolated tangent made perpendicular to n again; bitangent = cross(n, t) * handedness.
        vec3 t = f.tangent.xyz - n * dot(n, f.tangent.xyz);
        if (dot(t, t) > 1e-12) {
            t = normalize(t);
            vec3 b = cross(n, t) * (f.tangent.w < 0.0 ? -1.0 : 1.0);
            vec3 tn = f.normalMap * 2.0 - 1.0;
            tn.xy *= f.normalMapDepth;
            n = normalize(t * tn.x + b * tn.y + n * tn.z);
        }
    }
    vec3 v = normalize(u_cameraPos.xyz - v_worldPos);

    bool receive = u_shadowParams.x > 0.5;
    vec3 sun = u_lightColor.rgb * u_lightDir.w;
    if (receive && dot(sun, sun) > 0.0)
        sun *= csmShadow(v_worldPos, n);
    vec3 color = shade(n, v, -u_lightDir.xyz, sun, albedo, f0, metallic, roughness);
    for (int i = 0; i < MAX_POINT_LIGHTS; ++i) {
        vec3 toLight = u_pointPosRange[i].xyz - v_worldPos;
        float d = length(toLight);
        vec3 light = u_pointColor[i].rgb * falloff(d, u_pointPosRange[i].w);
        if (receive && u_pointShadow[i].x >= 0.0 && dot(light, light) > 0.0)
            light *= pointShadow(u_pointShadow[i], -toLight, n);
        color += shade(n, v, toLight / max(d, 1e-4), light, albedo, f0, metallic, roughness);
    }

    // Ambient, no IBL: diffuse ambient plus a flat specular term weighted by the roughness-aware Schlick Fresnel
    // F = F0 + (max(1 - roughness, F0) - F0) * (1 - N.V)^5, so metals (no diffuse) still show ambient.
    float nv = max(dot(n, v), 0.0);
    vec3 fAmbient = f0 + (max(vec3_splat(1.0 - roughness), f0) - f0) * pow(saturate(1.0 - nv), 5.0);
    color += ((vec3_splat(1.0) - fAmbient) * (1.0 - metallic) * albedo + fAmbient) * u_ambient.rgb * f.ao;

    color += f.emission;

    // Linear HDR: the post chain (post_process.h) tonemaps and encodes to sRGB.
    gl_FragColor = vec4(fogged(color, v_worldPos), alpha);
}
