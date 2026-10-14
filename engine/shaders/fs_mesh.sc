$input v_normal, v_worldPos, v_texcoord0

#include <bgfx_shader.sh>

#define MAX_POINT_LIGHTS 8 // kMaxPointLights in renderer.h
#define PI 3.14159265

SAMPLER2D(s_baseColor, 0);         // sRGB, sampled as linear; white if the material has none
SAMPLER2D(s_metallicRoughness, 1); // linear, glTF packing: G = roughness, B = metallic; white if none

uniform vec4 u_baseColor;  // linear RGBA, times the base color texture
uniform vec4 u_emissive;   // rgb: linear, added after lighting
uniform vec4 u_material;   // x: metallic, y: roughness, z: alpha cutoff (-1 = none), w: 1 to output alpha (blend)
uniform vec4 u_cameraPos;  // xyz: world position of the eye
uniform vec4 u_lightDir;   // xyz: world direction the light travels; w: 1 if the scene has a light, else 0
uniform vec4 u_lightColor; // rgb: light color * intensity
uniform vec4 u_ambient;    // rgb: scene ambient color * intensity
uniform vec4 u_pointPosRange[MAX_POINT_LIGHTS]; // xyz: world position; w: range (unused slots: 1)
uniform vec4 u_pointColor[MAX_POINT_LIGHTS];    // rgb: color * intensity (unused slots: 0)
uniform vec4 u_fogColor;  // rgb: linear fog color; w: 1 if fog is on
uniform vec4 u_fogParams; // x: density, y: start distance

// Windowed inverse square, as pointLightFalloff in renderer.cpp: saturate(1 - (d/r)^4)^2 / (d^2 + 1).
float falloff(float d, float range)
{
    float ratio = d / range;
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window / (d * d + 1.0);
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

void main()
{
    vec4 base = u_baseColor * texture2D(s_baseColor, v_texcoord0);
    if (base.a < u_material.z)
        discard;
    vec4 mr = texture2D(s_metallicRoughness, v_texcoord0);
    float metallic = saturate(u_material.x * mr.b);
    float roughness = clamp(u_material.y * mr.g, 0.04, 1.0); // a 0 roughness highlight is infinitely thin: floored
    vec3 albedo = base.rgb;
    vec3 f0 = mix(vec3_splat(0.04), albedo, metallic);

    vec3 n = normalize(v_normal);
    vec3 v = normalize(u_cameraPos.xyz - v_worldPos);

    vec3 color = shade(n, v, -u_lightDir.xyz, u_lightColor.rgb * u_lightDir.w, albedo, f0, metallic, roughness);
    for (int i = 0; i < MAX_POINT_LIGHTS; ++i) {
        vec3 toLight = u_pointPosRange[i].xyz - v_worldPos;
        float d = length(toLight);
        vec3 light = u_pointColor[i].rgb * falloff(d, u_pointPosRange[i].w);
        color += shade(n, v, toLight / max(d, 1e-4), light, albedo, f0, metallic, roughness);
    }

    // Ambient, no IBL: diffuse ambient plus a flat specular term weighted by the roughness-aware Schlick Fresnel
    // F = F0 + (max(1 - roughness, F0) - F0) * (1 - N.V)^5, so metals (no diffuse) still show ambient.
    float nv = max(dot(n, v), 0.0);
    vec3 fAmbient = f0 + (max(vec3_splat(1.0 - roughness), f0) - f0) * pow(saturate(1.0 - nv), 5.0);
    color += ((vec3_splat(1.0) - fAmbient) * (1.0 - metallic) * albedo + fAmbient) * u_ambient.rgb;

    color += u_emissive.rgb;

    // Exponential-squared distance fog, as fogFactor in post_process.cpp: 1 - exp(-(density * max(d - start, 0))^2).
    if (u_fogColor.w > 0.5) {
        float fd = u_fogParams.x * max(length(u_cameraPos.xyz - v_worldPos) - u_fogParams.y, 0.0);
        color = mix(color, u_fogColor.rgb, 1.0 - exp(-fd * fd));
    }
    // Linear HDR: the post chain (post_process.h) tonemaps and encodes to sRGB.
    gl_FragColor = vec4(color, u_material.w > 0.5 ? base.a : 1.0);
}
