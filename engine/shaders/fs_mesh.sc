$input v_normal, v_tangent, v_worldPos, v_texcoord0, v_color0, v_lightmapUv

// The mesh fragment template: Kast's built-in shader as is, and every .kshader's fragment() at the //@user line
// (shader.cpp generateShader; see docs/engine/shaders.html). With KAST_SHADOW it is the .kshader's shadow pass shader.

#include <bgfx_shader.sh>
#include <mesh.sh>

#define MAX_LOCAL_LIGHTS 8 // kMaxLocalLights in renderer.h: point, spot and area lights per draw
#define MAX_CASCADES 4     // kMaxShadowCascades in scene.h
#define MAX_SHADOW_TILES 8 // the cascades, then kMaxShadowedLocalLights in renderer.h
#define CSM_TEXEL vec2(1.0 / 4096.0, 1.0 / 8192.0) // one texel of the shadow atlas: kShadowAtlasWidth x Height in shadow.h
#define CSM_TILE_SIZE 2048.0            // kCascadeSize in shadow.h: texels per side of one atlas tile
#define POINT_SHADOW_SIZE 1024.0        // kPointShadowSize in shadow.h
#define POINT_SHADOW_NEAR 0.05          // kPointShadowNear in shadow.h
// Constants rather than #defines: a .kshader param may take these names (only the built-ins are reserved).
static const float AREA_SHADOW_TAN = 1.7320508; // tan(kAreaShadowFovDegrees / 2) in shadow.h
static const int LTC_SIZE = 16;                 // kLtcSize in renderer.h
// u_localColor.w: the light's type (0: point).
static const float LIGHT_SPOT = 1.0;
static const float LIGHT_AREA = 2.0;
static const float LIGHT_AREA_TWO_SIDED = 3.0;

SAMPLER2D(s_baseColor, 0);         // sRGB, sampled as linear; white if the material has none
// Linear, glTF's ORM packing: R = occlusion, G = roughness, B = metallic (u_materialOrm says which it holds; renderer.cpp
// setMaterialState packs separate occlusion and metallic-roughness images into one); white if none.
SAMPLER2D(s_metallicRoughness, 1);
// The shadow atlas, 2x4 tiles (shadow.h): directional light cascades (cascade i at column i % 2, row i / 2), then spot /
// area light tiles (u_csmMtx index i, 4..7, at column i % 2, row i / 2).
SAMPLER2DSHADOW(s_csm, 2);
SAMPLERCUBESHADOW(s_pointShadow0, 3); // shadowed point light cubes: depth of each face's 90 degree projection
SAMPLERCUBESHADOW(s_pointShadow1, 4);
SAMPLERCUBESHADOW(s_pointShadow2, 5);
SAMPLERCUBESHADOW(s_pointShadow3, 6);
SAMPLER2D(s_normal, 7);            // linear tangent-space normal, glTF / OpenGL (+Y green); flat (0.5, 0.5, 1) if none
// The environment the ambient comes from (ibl.h): a GGX-prefiltered cube, mip i of roughness i / u_envSpecular.w,
// sampled with Kast's world z flipped (as fs_sky.sc); black if none. Stage 8 was the occlusion texture's until it moved
// into s_metallicRoughness's R: every stage is taken (D3D11's 16).
SAMPLERCUBE(s_env, 8);
SAMPLER2D(s_emissive, 9);          // sRGB, sampled as linear, times u_emissive; white if none
// Stages 10..13: a .kshader's texture2D params (shader.cpp generateShader); 12 and 13 are the lightmap's (s_lightmap,
// s_lightmapDir below) in shaders with at most two, 11 VoxelGI's (s_voxelGi) in shaders with at most one; 10 is SDFGI's
// (s_sdfgi) in the built-in shader's SDFGI variant.
SAMPLER2D(s_kastScreen, 14); // SCREEN_TEXTURE: the opaque scene's HDR linear color (Renderer::render copies it)
SAMPLER2D(s_kastDepth, 15);  // DEPTH_TEXTURE: the opaque scene's depth (D3D 0..1); linearDepth() makes it meters

uniform vec4 u_materialExt; // x: normal scale, y: occlusion strength, z: 1 if a normal texture is bound, w: 1 if unlit
// x: 1 if s_metallicRoughness holds the metallic-roughness texture (G, B; else they read 1), y: 1 if it holds the
// occlusion texture (R; else it reads 1)
uniform vec4 u_materialOrm;
uniform vec4 u_lightDir;   // xyz: world direction the light travels; w: 1 if the scene has a light, else 0
uniform vec4 u_lightColor; // rgb: light color * intensity
uniform vec4 u_ambient;    // rgb: scene ambient color * intensity: the flat ambient (no environment)
// The environment (renderer.cpp setDrawState): the sky's (tinted by the ambient color * intensity) or a probe's (times its
// intensity). u_envSh: its spherical harmonics, already tinted (ibl.h Sh9); u_envSpecular: rgb: the specular tint, w:
// s_env's roughness 1 mip (0: no environment, the flat u_ambient instead); u_envBox: [0] xyz: the probe's center, w: 1 if
// box-projected; [1] xyz: half its box size.
uniform vec4 u_envSh[9];
uniform vec4 u_envSpecular;
uniform vec4 u_envBox[2];
// Global illumination (docs/engine/lightmaps.html; renderer.cpp setDrawState): x: where the diffuse ambient comes from
// (lightmap.h DiffuseGi): 0 the environment as above, 1 the lightmap (s_lightmap, s_lightmapDir at v_lightmapUv), 2
// u_giSh, the probe grid sampled at the mesh (Sh9, as u_envSh), 3 a VoxelGI volume (u_voxelGi below), 4 SDFGI's probes
// (u_sdfgi below); y: the lightmaps' and grid's intensity; z: 1 to bend lightmap light by the normal (the directional
// term). The specular stays the environment's, but inside a VoxelGI volume (u_voxelGi[3].w) or on a rough SDFGI mesh
// (u_sdfgi[5].z, KAST_SDFGI only).
uniform vec4 u_gi;
uniform vec4 u_giSh[9];
// The local light slots (Renderer::render setDrawState), nearest the mesh first. Unused slots: black, range 1.
uniform vec4 u_localPosRange[MAX_LOCAL_LIGHTS]; // xyz: world position (area: the rect's center); w: range
uniform vec4 u_localColor[MAX_LOCAL_LIGHTS];    // rgb: color * intensity; w: type: 0 point, LIGHT_SPOT, LIGHT_AREA(_TWO_SIDED)
uniform vec4 u_localAxis[MAX_LOCAL_LIGHTS];     // spot: xyz: the direction it shines; area: xyz: half its width
                                                // along its X, w: z of half its height along its Y
// x: shadow: point light cube 0..3, spot / area light u_csmMtx tile 4..7, -1 none; y: bias; point: z: range;
// spot: z, w: cosines of the inner and outer angles; area: zw: xy of half its height along its Y.
uniform vec4 u_localShadow[MAX_LOCAL_LIGHTS];
uniform vec4 u_fogColor;  // rgb: linear fog color; w: 1 if fog is on
uniform vec4 u_fogParams; // x: density, y: start distance
uniform mat4 u_csmMtx[MAX_SHADOW_TILES]; // world to (atlas u, v, depth) per cascade, then (projective) per spot / area tile
uniform vec4 u_csmSplits;             // far view depth of each cascade (unused cascades: the last's)
uniform vec4 u_csmBias[MAX_CASCADES]; // x: normal offset (meters), y: depth bias, per cascade
uniform vec4 u_shadowParams;          // x: 1 if this mesh receives shadows, y: 1 if the directional light casts them

// The area light table (tools/ltc_fit): see ltc_table.sh and ltcLookup. A shader constant (no sampler stage is
// free), so it costs compile time: kept small, and left out of the depth-only shadow pass.
#ifndef KAST_SHADOW
static const vec4 s_ltcTable[LTC_SIZE * LTC_SIZE * 2] = {
#include <ltc_table.sh>
};
#endif

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

// A spot light's cone edge, as spotConeFalloff in renderer.cpp: smoothstep from cos(outer) (0) to cos(inner) (1).
float spotCone(float cosAngle, float cosInner, float cosOuter)
{
    float t = saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4));
    return t * t * (3.0 - 2.0 * t);
}

// A spot or area light's shadow: its perspective tile of the atlas (u_csmMtx[tile], 4..7), from the light out to its
// range. toLight: unit, toward the light (the receiver moves bias meters along it); d: the distance; tanHalfFov: of the
// tile's projection. (2 * radius + 1)^2 PCF taps spread texels apart, kept inside the tile: 1 lit, 0 in shadow; lit
// outside the tile's frustum.
float tileShadow(float tile, vec3 worldPos, vec3 toLight, float d, vec3 n, float bias, float tanHalfFov, int radius,
                 float spread)
{
    float texel = 2.0 * d * tanHalfFov / CSM_TILE_SIZE; // meters per texel at that distance (roughly)
    vec4 p = mul(u_csmMtx[int(tile)], vec4(worldPos + toLight * bias + n * texel, 1.0));
    if (p.w <= 0.0)
        return 1.0;
    vec3 coord = p.xyz / p.w;
    float i = tile - float(MAX_CASCADES);
    vec2 lo = vec2(0.5 * (i - 2.0 * floor(i / 2.0)), 0.25 * (2.0 + floor(i / 2.0)));
    vec2 hi = lo + vec2(0.5, 0.25);
    if (coord.x < lo.x || coord.y < lo.y || coord.x > hi.x || coord.y > hi.y || coord.z >= 1.0)
        return 1.0;
    float lit = 0.0;
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            vec2 uv = clamp(coord.xy + vec2(x, y) * spread * CSM_TEXEL, lo + CSM_TEXEL, hi - CSM_TEXEL);
            lit += shadow2D(s_csm, vec3(uv, coord.z));
        }
    float taps = float(2 * radius + 1);
    return lit / (taps * taps);
}

// The LTC table bilinearly at (roughness, sqrt(1 - N.V)) (Heitz et al. 2016's parametrization): minv: the inverse LTC
// matrix (m00, m02, m20, m22, with m11 = 1) in the frame where the view is in the XZ plane; albedo: the GGX albedo is
// F0 * x + y.
void ltcLookup(float roughness, float nv, out vec4 minv, out vec2 albedo)
{
#ifdef KAST_SHADOW
    minv = vec4(1.0, 0.0, 0.0, 1.0);
    albedo = vec2(1.0, 0.0);
#else
    vec2 uv = vec2(roughness, sqrt(1.0 - nv)) * float(LTC_SIZE - 1);
    vec2 base = min(floor(uv), vec2_splat(float(LTC_SIZE - 2)));
    vec2 f = uv - base;
    int i00 = (int(base.y) * LTC_SIZE + int(base.x)) * 2; // two vec4 per entry
    int i01 = i00 + LTC_SIZE * 2;
    minv = mix(mix(s_ltcTable[i00], s_ltcTable[i00 + 2], f.x), mix(s_ltcTable[i01], s_ltcTable[i01 + 2], f.x), f.y);
    albedo = mix(mix(s_ltcTable[i00 + 1].xy, s_ltcTable[i00 + 3].xy, f.x),
                 mix(s_ltcTable[i01 + 1].xy, s_ltcTable[i01 + 3].xy, f.x), f.y);
#endif
}

// Of a polygon edge from a to b (not unit): the z of its vector form factor, times 2 pi.
float edgeIntegral(vec3 a, vec3 b)
{
    a = a * inversesqrt(max(dot(a, a), 1e-12));
    b = b * inversesqrt(max(dot(b, b), 1e-12));
    float x = clamp(dot(a, b), -1.0, 1.0);
    float ratio = x > 0.99999 ? 1.0 : acos(x) * inversesqrt(max(1.0 - x * x, 1e-12));
    return cross(a, b).z * ratio;
}

// Adds edge a -> b's part above the horizon (z >= 0) to sum; an edge crossing it ends at (or starts from) the crossing,
// noted as exitPoint (or entryPoint) for the edge along the horizon that closes the clipped polygon.
void clippedEdge(vec3 a, vec3 b, inout float sum, inout vec3 exitPoint, inout vec3 entryPoint, inout float clipped)
{
    if (a.z >= 0.0 && b.z >= 0.0) {
        sum += edgeIntegral(a, b);
    } else if (a.z >= 0.0 || b.z >= 0.0) {
        vec3 cut = a + (b - a) * (a.z / (a.z - b.z));
        clipped = 1.0;
        if (a.z >= 0.0) {
            sum += edgeIntegral(a, cut);
            exitPoint = cut;
        } else {
            sum += edgeIntegral(cut, b);
            entryPoint = cut;
        }
    }
}

// The integral over the rect (corners c0..c3 relative to the surface, clockwise as seen from the side that lights)
// of the cosine distribution around normal n transformed by minv (ltcLookup's frame of n and view v; (1, 0, 0, 1):
// the clamped cosine itself, the diffuse form factor), clipped to the horizon: 0..1, 0 seen from behind unless
// twoSided. As rectFormFactor in renderer.cpp.
float ltcIntegrate(vec3 n, vec3 v, vec4 minv, vec3 c0, vec3 c1, vec3 c2, vec3 c3, bool twoSided)
{
    vec3 t1 = v - n * dot(v, n);
    if (dot(t1, t1) < 1e-8) // looking straight down n: any tangent
        t1 = cross(abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n);
    t1 = normalize(t1);
    vec3 t2 = cross(n, t1);
    vec3 q0 = vec3(dot(t1, c0), dot(t2, c0), dot(n, c0));
    vec3 q1 = vec3(dot(t1, c1), dot(t2, c1), dot(n, c1));
    vec3 q2 = vec3(dot(t1, c2), dot(t2, c2), dot(n, c2));
    vec3 q3 = vec3(dot(t1, c3), dot(t2, c3), dot(n, c3));
    q0 = vec3(minv.x * q0.x + minv.y * q0.z, q0.y, minv.z * q0.x + minv.w * q0.z);
    q1 = vec3(minv.x * q1.x + minv.y * q1.z, q1.y, minv.z * q1.x + minv.w * q1.z);
    q2 = vec3(minv.x * q2.x + minv.y * q2.z, q2.y, minv.z * q2.x + minv.w * q2.z);
    q3 = vec3(minv.x * q3.x + minv.y * q3.z, q3.y, minv.z * q3.x + minv.w * q3.z);
    float sum = 0.0;
    float clipped = 0.0;
    vec3 exitPoint = vec3_splat(0.0);
    vec3 entryPoint = vec3_splat(0.0);
    clippedEdge(q0, q1, sum, exitPoint, entryPoint, clipped);
    clippedEdge(q1, q2, sum, exitPoint, entryPoint, clipped);
    clippedEdge(q2, q3, sum, exitPoint, entryPoint, clipped);
    clippedEdge(q3, q0, sum, exitPoint, entryPoint, clipped);
    if (clipped > 0.5)
        sum += edgeIntegral(exitPoint, entryPoint);
    sum /= 2.0 * PI;
    return twoSided ? abs(sum) : max(sum, 0.0);
}

// Area light slot i at the surface point p: its corners relative to p, in right-hand order around its +Z (clockwise
// as seen from the -Z side it lights).
void areaCorners(int i, vec3 p, out vec3 c0, out vec3 c1, out vec3 c2, out vec3 c3)
{
    vec3 center = u_localPosRange[i].xyz - p;
    vec3 halfWidth = u_localAxis[i].xyz;
    vec3 halfHeight = vec3(u_localShadow[i].zw, u_localAxis[i].w);
    c0 = center - halfWidth - halfHeight;
    c1 = center + halfWidth - halfHeight;
    c2 = center + halfWidth + halfHeight;
    c3 = center - halfWidth + halfHeight;
}

// Area light slot i's range window from its center (as falloff's, without the inverse square: the form factor falls
// off itself) times its shadow at p.
float areaVisibility(int i, vec3 p, vec3 n, bool receive)
{
    vec3 offset = u_localPosRange[i].xyz - p;
    float d = length(offset);
    float ratio = d / u_localPosRange[i].w;
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    float visible = window * window;
    if (receive && u_localShadow[i].x >= 0.0 && visible > 0.0)
        visible *= tileShadow(u_localShadow[i].x, p, offset / max(d, 1e-4), d, n, u_localShadow[i].y, AREA_SHADOW_TAN, 2,
                              1.5);
    return visible;
}

// Area light slot i's point closest to p, unit direction from p.
vec3 areaClosestDirection(int i, vec3 p)
{
    vec3 halfWidth = u_localAxis[i].xyz;
    vec3 halfHeight = vec3(u_localShadow[i].zw, u_localAxis[i].w);
    vec3 offset = p - u_localPosRange[i].xyz;
    vec3 closest = u_localPosRange[i].xyz + halfWidth * clamp(dot(offset, halfWidth) / dot(halfWidth, halfWidth), -1.0, 1.0)
                 + halfHeight * clamp(dot(offset, halfHeight) / dot(halfHeight, halfHeight), -1.0, 1.0);
    vec3 toClosest = closest - p;
    return toClosest * inversesqrt(max(dot(toClosest, toClosest), 1e-12));
}

// Spot light slot i's shadow at p (toLight: unit; d: the distance): its tile is its outer cone wide.
float spotShadow(int i, vec3 p, vec3 toLight, float d, vec3 n)
{
    float cosOuter = u_localShadow[i].w;
    float tanOuter = sqrt(max(1.0 - cosOuter * cosOuter, 0.0)) / max(cosOuter, 1e-4);
    return tileShadow(u_localShadow[i].x, p, toLight, d, n, u_localShadow[i].y, tanOuter, 1, 1.0);
}

// Area light slot i at p, with Linearly Transformed Cosines (Heitz et al. 2016): diffuse from the clamped cosine's form
// factor of the rect, GGX specular from the fitted LTC's, each times light (color * intensity * areaVisibility; color
// * intensity is the rect's radiance in shade()'s units, see AreaLight in scene.h). The specular's Fresnel comes with
// the table's albedo (F0 * x + y); the diffuse's 1 - F is Schlick's at N.V, an approximation of shade()'s per-light V.H.
vec3 areaShade(int i, vec3 p, vec3 n, vec3 v, vec3 light, vec3 albedo, vec3 f0, float metallic, float roughness)
{
    vec3 c0, c1, c2, c3;
    areaCorners(i, p, c0, c1, c2, c3);
    bool twoSided = u_localColor[i].w == LIGHT_AREA_TWO_SIDED;
    float nv = saturate(dot(n, v));
    vec4 minv;
    vec2 brdf;
    ltcLookup(roughness, nv, minv, brdf);
    float diffuse = ltcIntegrate(n, v, vec4(1.0, 0.0, 0.0, 1.0), c0, c1, c2, c3, twoSided);
    float specular = ltcIntegrate(n, v, minv, c0, c1, c2, c3, twoSided);
    vec3 f = f0 + (vec3_splat(1.0) - f0) * pow(1.0 - nv, 5.0);
    vec3 kd = (vec3_splat(1.0) - f) * (1.0 - metallic);
    return (kd * albedo * diffuse + (f0 * brdf.x + brdf.y) * specular) * light;
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

// The environment's diffuse light at unit normal n: irradiance / pi from its spherical harmonics (shIrradiance in ibl.h).
vec3 kastSh9(vec3 n)
{
    vec3 e = u_envSh[0].rgb + u_envSh[1].rgb * n.y + u_envSh[2].rgb * n.z + u_envSh[3].rgb * n.x
           + u_envSh[4].rgb * (n.x * n.y) + u_envSh[5].rgb * (n.y * n.z) + u_envSh[6].rgb * (3.0 * n.z * n.z - 1.0)
           + u_envSh[7].rgb * (n.x * n.z) + u_envSh[8].rgb * (n.x * n.x - n.y * n.y);
    return max(e, vec3_splat(0.0));
}

// The split-sum's GGX BRDF integral, Karis' analytic fit (envBrdf in ibl.h): specular reflectance = F0 * x + y.
vec2 kastEnvBrdf(float roughness, float nv)
{
    vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return saturate(vec2(-1.04, 1.04) * a004 + r.zw); // the fit strays a little past 0..1 at its extremes
}

// Where reflection ray r from p samples the environment (boxProjectedDirection in ibl.h): box-projected, the direction
// from the probe's center to where r leaves its box (r itself from outside the box); the sky's, r.
vec3 kastEnvDirection(vec3 r, vec3 p)
{
    vec3 lo = u_envBox[0].xyz - u_envBox[1].xyz;
    vec3 hi = u_envBox[0].xyz + u_envBox[1].xyz;
    if (u_envBox[0].w < 0.5 || any(lessThan(p, lo)) || any(greaterThan(p, hi)))
        return r;
    vec3 t = abs(mix(lo, hi, step(vec3_splat(0.0), r)) - p) / max(abs(r), vec3_splat(1e-5));
    return p + r * min(min(t.x, t.y), t.z) - u_envBox[0].xyz;
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

// How much of a surface's own light distance fog leaves at worldPos (fogged's 1 - fog): scales its G-data's indirect light.
float kastFogKeep(vec3 worldPos)
{
    if (u_fogColor.w < 0.5)
        return 1.0;
    float fd = u_fogParams.x * max(length(u_cameraPos.xyz - worldPos) - u_fogParams.y, 0.0);
    return exp(-fd * fd);
}

// G-data attachment 1 (post_process.h PostProcess): world normal n as view space, octahedral in rg (octEncode in
// post_process.cpp), perceptual roughness in b, a 1 (a surface; the clear's 0 is none).
vec4 kastGNormal(vec3 n, float roughness)
{
    vec3 v = mul(u_view, vec4(n, 0.0)).xyz;
    v /= abs(v.x) + abs(v.y) + abs(v.z);
    vec2 p = v.z >= 0.0 ? v.xy : (vec2_splat(1.0) - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return vec4(p * 0.5 + 0.5, roughness, 1.0);
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
    vec2 screenUv;       // FRAG_COORD.xy over the view's size: (0, 0) top-left, where SCREEN_TEXTURE and DEPTH_TEXTURE sample
};

// What light() reads and writes, once per light. Its parameter is named kast_f too, so NORMAL, ALBEDO, METALLIC and
// ROUGHNESS mean these fields inside light() (and KastFragment's in fragment()).
struct KastLight {
    vec3 lightDirection; // world, unit: from the surface toward the light (area: its closest point on the rect)
    vec3 lightColor;     // color * intensity
    float attenuation;   // 0..1: falloff (point: distance; spot: distance * cone; area: see main()) times shadow
    vec3 normal;         // world, unit: after fragment() and the normal map
    vec3 view;           // world, unit: from the surface toward the camera
    vec3 albedo;
    float metallic;      // 0..1
    float roughness;     // 0.04..1
    vec3 diffuse;        // accumulated over the lights, from 0; the color is diffuse + specular, then ambient and fog
    vec3 specular;
};

// Built-ins: fragment() inputs and outputs (fields of its kast_f) and texture helpers; mesh.sh has those of every stage.
// The names are reserved: a .kshader param may not take one (shaderBuiltinNames in shader.cpp lists them).
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
#define SCREEN_UV kast_f.screenUv
#define SCREEN_TEXTURE s_kastScreen
#define DEPTH_TEXTURE s_kastDepth
#define LIGHT_DIRECTION kast_f.lightDirection
#define LIGHT_COLOR kast_f.lightColor
#define ATTENUATION kast_f.attenuation
#define VIEW kast_f.view
#define DIFFUSE_LIGHT kast_f.diffuse
#define SPECULAR_LIGHT kast_f.specular
vec4 sampleBaseColor(vec2 uv) { return texture2D(s_baseColor, uv); }
// G = roughness, B = metallic (R: the occlusion if packed with it); white without a metallic-roughness texture.
vec4 sampleMetallicRoughness(vec2 uv) { return u_materialOrm.x > 0.5 ? texture2D(s_metallicRoughness, uv) : vec4_splat(1.0); }
// A tangent-space normal as stored (0..1) with Z rebuilt from X and Y: BC5 normal maps (cooked PNG / JPG) keep only X and Y
// and read Z as 0. The same for full ones, which are unit length too.
vec3 unpackNormalMap(vec4 stored)
{
    vec2 xy = stored.xy * 2.0 - 1.0;
    return vec3(stored.xy, sqrt(saturate(1.0 - dot(xy, xy))) * 0.5 + 0.5);
}
vec4 sampleNormal(vec2 uv) { return vec4(unpackNormalMap(texture2D(s_normal, uv)), 1.0); }
// R = ambient occlusion (the other channels: whatever its image holds); white without an occlusion texture.
vec4 sampleOcclusion(vec2 uv) { return u_materialOrm.y > 0.5 ? texture2D(s_metallicRoughness, uv) : vec4_splat(1.0); }
vec4 sampleEmissive(vec2 uv) { return texture2D(s_emissive, uv); }
// A DEPTH_TEXTURE value as the view depth in meters (positive, along the camera's forward), as -VIEW_POSITION.z.
float linearDepth(float depth)
{
    vec4 p = mul(u_invProj, vec4(0.0, 0.0, depth, 1.0));
    return -p.z / p.w;
}

//@user
#ifndef KAST_FRAGMENT
void fragment(inout KastFragment kast_f) {}
#endif

// Volumetric fog (volumetric_fog.h) of a blended draw (u_volFog[0].w 1; the opaque scene gets it in VolumetricFog's
// composite): what the froxel grid's atlas holds in front of the fragment, on SCREEN_TEXTURE's stage, which the built-in
// shader and .kshaders that don't sample SCREEN_TEXTURE (KAST_VOLUMETRIC_FOG, shader.cpp) leave free. Alpha-blended
// surfaces take its transmittance and in-scattered light, additive ones only the transmittance (as fogged()).
#if !defined(KAST_FRAGMENT) || defined(KAST_VOLUMETRIC_FOG)
#include <volumetric_fog.sh>
uniform vec4 u_volFog[2]; // VolumetricFog::drawParams: [0] xyz the camera's forward, w 1 on; [1] y maxDistance
vec3 kastVolumetricFog(vec3 color, vec3 worldPos, vec2 pixel)
{
    if (u_volFog[0].w < 0.5)
        return color;
    vec4 fog = kastFroxelFog(s_kastScreen, (pixel - u_viewRect.xy) * u_viewTexel.xy,
                             dot(worldPos - u_cameraPos.xyz, u_volFog[0].xyz), u_volFog[1].y, pixel);
    return u_material.w > 1.5 ? color * fog.a : color * fog.a + fog.rgb;
}
#else
vec3 kastVolumetricFog(vec3 color, vec3 worldPos, vec2 pixel) { return color; }
#endif

// The lightmap (lightmap.h): rgb irradiance / pi, and the direction it mostly came from (xyz) with its half-Lambert at
// the baked surface's normal (w). On stages a .kshader's third and fourth texture2D params take: shaders with those get
// no lightmap (KAST_TEXTURE_PARAMS, shader.cpp; the renderer gives their draws the probe grid instead).
#if !defined(KAST_TEXTURE_PARAMS) || KAST_TEXTURE_PARAMS <= 2
#define KAST_HAS_LIGHTMAP
SAMPLER2D(s_lightmap, 12);
SAMPLER2D(s_lightmapDir, 13);
#endif

// The probe grid's diffuse light at unit normal n (u_giSh, as kastSh9), times its intensity.
vec3 kastGiSh(vec3 n)
{
    vec3 e = u_giSh[0].rgb + u_giSh[1].rgb * n.y + u_giSh[2].rgb * n.z + u_giSh[3].rgb * n.x
           + u_giSh[4].rgb * (n.x * n.y) + u_giSh[5].rgb * (n.y * n.z) + u_giSh[6].rgb * (3.0 * n.z * n.z - 1.0)
           + u_giSh[7].rgb * (n.x * n.z) + u_giSh[8].rgb * (n.x * n.x - n.y * n.y);
    return max(e, vec3_splat(0.0)) * u_gi.y;
}

// VoxelGI (voxel_gi.h, docs/engine/voxelgi.html): the radiance volume the draw's box selected, relit this frame (rgb
// premultiplied by opacity in a, mip i of 2^i voxels). On the stage a .kshader's second texture2D param takes: shaders
// with two or more get none (KAST_TEXTURE_PARAMS; the renderer gives their draws the light below VoxelGI instead).
#if !defined(KAST_TEXTURE_PARAMS) || KAST_TEXTURE_PARAMS <= 1
#define KAST_HAS_VOXELGI
static const float VOXEL_EDGE = 4.0;           // kVoxelGiEdgeVoxels in voxel_gi.h
static const float VOXEL_MIN_APERTURE = 0.04;  // kVoxelGiMinAperture in voxel_gi.h
#define MAX_VOXEL_CONES 9                      // kMaxVoxelGiCones in voxel_gi.h
SAMPLER3D(s_voxelGi, 11);
// Renderer::render setDrawState: [0] xyz: the grid's min corner, w: voxel size (m); [1] xyz: 1 / its extent (m), w:
// intensity; [2] x: its last mip, y: diffuse cones, z: step scale, w: specular steps at most; [3] x: the diffuse cones'
// aperture (tan of the half angle), y: 1 if the light without the volume is the probe grid's (u_giSh), z: diffuse steps
// at most, w: 1 if a volume is bound (its specular replaces the environment's).
uniform vec4 u_voxelGi[4];
uniform vec4 u_voxelCones[MAX_VOXEL_CONES]; // voxelGiCones in voxel_gi.h: xyz in the tangent frame (z: the normal), w weight

// voxelGiEdgeWeight in voxel_gi.cpp: 1 inside, 0 at (and outside) the grid's faces, linear over its last VOXEL_EDGE voxels.
float kastVoxelEdge(vec3 p)
{
    vec3 lo = u_voxelGi[0].xyz;
    vec3 hi = lo + vec3_splat(1.0) / u_voxelGi[1].xyz;
    vec3 d = min(p - lo, hi - p);
    return saturate(min(min(d.x, d.y), d.z) / (VOXEL_EDGE * u_voxelGi[0].w));
}

// The volume front to back along a cone from p (unit dir; aperture: tan of its half angle) off a surface of unit normal
// n, as voxelConeSteps in voxel_gi.cpp steps it, until it leaves the grid, is 95% blocked or took maxSteps: rgb the light
// it gathered, a how much of the view it blocked. Each sample is lifted off the surface by the cone's width there,
// so the mip voxels holding the surface itself (around most of a wide cone's samples) don't shade it.
vec4 kastVoxelCone(vec3 p, vec3 n, vec3 dir, float aperture, float maxSteps)
{
    float voxel = u_voxelGi[0].w;
    vec4 sum = vec4_splat(0.0);
    float t = voxel;
    for (int i = 0; i < 128; ++i) {
        if (float(i) >= maxSteps || sum.a >= 0.95)
            break;
        float diameter = max(voxel, 2.0 * aperture * t);
        vec3 uvw = (p + dir * t + n * diameter - u_voxelGi[0].xyz) * u_voxelGi[1].xyz;
        if (any(lessThan(uvw, vec3_splat(0.0))) || any(greaterThan(uvw, vec3_splat(1.0))))
            break;
        float lod = min(log2(diameter / voxel), u_voxelGi[2].x);
        vec4 s = texture3DLod(s_voxelGi, uvw, lod);
        float a = min(s.a * exp2(lod), 1.0); // a voxel-thin surface fills 1 / 2^lod of a mip voxel: count it as filling it
        s = vec4(s.rgb * (a / max(s.a, 1e-4)), a);
        sum += (1.0 - sum.a) * s;
        t += diameter * u_voxelGi[2].z;
    }
    return sum;
}

// What a cone that leaves the volume unblocked sees along d: the environment's diffuse light (its harmonics, for these
// wide cones), or the flat ambient.
vec3 kastVoxelEnvironment(vec3 d)
{
    return u_envSpecular.w > 0.0 ? kastSh9(d) : u_ambient.rgb;
}

// The volume's diffuse light at p (unit normal n): the preset's cones around n, weighted, each seeing past the volume
// what it didn't block; blended toward fallback (the light without the volume) at the box's faces.
vec3 kastVoxelDiffuse(vec3 n, vec3 p, vec3 fallback)
{
    float w = kastVoxelEdge(p);
    if (w <= 0.0)
        return fallback;
    vec3 t = normalize(cross(abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 b = cross(n, t);
    vec3 origin = p + n * u_voxelGi[0].w;
    vec3 sum = vec3_splat(0.0);
    for (int i = 0; i < MAX_VOXEL_CONES; ++i) {
        if (float(i) >= u_voxelGi[2].y)
            break;
        vec4 c = u_voxelCones[i];
        vec3 d = t * c.x + b * c.y + n * c.z;
        vec4 cone = kastVoxelCone(origin, n, d, u_voxelGi[3].x, u_voxelGi[3].z);
        sum += c.w * (cone.rgb * u_voxelGi[1].w + (1.0 - cone.a) * kastVoxelEnvironment(d));
    }
    return mix(fallback, sum, w);
}

// The volume's specular at p: one cone along the reflection as wide as the GGX lobe (voxelGiSpecularAperture), past it
// the environment's prefiltered cube (or the flat ambient), times the split sum's BRDF; blended toward fallback at the
// box's faces.
vec3 kastVoxelSpecular(vec3 n, vec3 v, vec3 p, float roughness, vec3 f0, float nv, vec3 fallback)
{
    float w = kastVoxelEdge(p);
    if (w <= 0.0)
        return fallback;
    vec3 r = reflect(-v, n);
    vec3 env = u_ambient.rgb;
    if (u_envSpecular.w > 0.0) {
        vec3 er = kastEnvDirection(r, p);
        env = textureCubeLod(s_env, vec3(er.x, er.y, -er.z), roughness * u_envSpecular.w).rgb * u_envSpecular.rgb;
    }
    vec4 cone = kastVoxelCone(p + n * u_voxelGi[0].w, n, r, max(roughness * roughness, VOXEL_MIN_APERTURE), u_voxelGi[2].w);
    vec2 brdf = kastEnvBrdf(roughness, nv);
    return mix(fallback, (cone.rgb * u_voxelGi[1].w + (1.0 - cone.a) * env) * (f0 * brdf.x + brdf.y), w);
}
#endif

// SDFGI (sdfgi.h, docs/engine/sdfgi.html): the cascades' probes, updated this frame, on stage 10. Only in the built-in
// shader's SDFGI variant (fs_mesh_sdfgi: this file compiled with KAST_SDFGI), which the renderer gives the draws that take
// SDFGI: every other draw runs code compiled exactly as before SDFGI. s_sdfgi: per probe 4 texels along x
// (cs_sdfgi_probe.sc: a0 rgb + a valid, then a1 per channel), at the global probe mod the probes per axis, the cascades
// stacked along z.
#ifdef KAST_SDFGI
#define KAST_HAS_SDFGI
static const float SDFGI_BLEND = 2.0;        // kSdfgiBlendProbes in sdfgi.h
static const float SDFGI_ROUGHNESS = 0.5;    // kSdfgiRoughness in sdfgi.h
static const float SDFGI_NORMAL_BIAS = 0.25; // kSdfgiNormalBias in sdfgi.h
SAMPLER3D(s_sdfgi, 10);
// SdfgiRenderer::bind: [0..3] per cascade: xyz its first probe (a global probe index), w the probe spacing (m); [4] xyz:
// probes per axis, w: cascades; [5] x: energy, y: 1 if the light past the last cascade is the probe grid's (u_giSh),
// z: 1 for rough reflections from SDFGI, w: 1 if the draw takes SDFGI.
uniform vec4 u_sdfgi[6];

// sdfgiCascadeWeight in sdfgi.cpp: how much the (biased) point q uses cascade c.
float kastSdfgiEdge(int c, vec3 q)
{
    vec3 local = q / u_sdfgi[c].w - u_sdfgi[c].xyz;
    vec3 e = min(local, u_sdfgi[4].xyz - vec3_splat(1.0) - local);
    return saturate(min(e.x, min(e.y, e.z)) / SDFGI_BLEND);
}

// Cascade c's probes around p (unit normal n) weighted as sdfgiProbeWeights in sdfgi.cpp: their harmonics' weighted
// mean into a0 and a1 (per channel). Returns the weights' sum (0: no valid probe).
float kastSdfgiCascade(int c, vec3 p, vec3 n, out vec3 a0, out vec3 ar, out vec3 ag, out vec3 ab)
{
    a0 = vec3_splat(0.0);
    ar = vec3_splat(0.0);
    ag = vec3_splat(0.0);
    ab = vec3_splat(0.0);
    float spacing = u_sdfgi[c].w;
    vec3 dims = u_sdfgi[4].xyz;
    vec3 first = u_sdfgi[c].xyz;
    vec3 local = clamp((p + n * (SDFGI_NORMAL_BIAS * spacing)) / spacing - first, vec3_splat(0.0), dims - vec3_splat(1.001));
    vec3 base = floor(local);
    vec3 f = local - base;
    float total = 0.0;
    for (int i = 0; i < 8; ++i) {
        vec3 corner = vec3(float(i - (i / 2) * 2), float((i / 2) - (i / 4) * 2), float(i / 4));
        vec3 q = first + base + corner;
        vec3 t = q - dims * floor(q / dims);
        ivec3 texel = ivec3(int(t.x) * 4, int(t.y), int(t.z) + c * int(dims.z));
        vec4 c0 = texelFetch(s_sdfgi, texel, 0);
        if (c0.a < 0.5)
            continue; // inside geometry, or no history yet
        vec3 tri = mix(vec3_splat(1.0) - f, f, corner);
        vec3 toProbe = normalize(q * spacing - p + vec3_splat(1e-5));
        float back = (dot(toProbe, n) + 1.0) * 0.5;
        float w = tri.x * tri.y * tri.z * (back * back + 0.05);
        a0 += w * c0.rgb;
        ar += w * texelFetch(s_sdfgi, texel + ivec3(1, 0, 0), 0).xyz;
        ag += w * texelFetch(s_sdfgi, texel + ivec3(2, 0, 0), 0).xyz;
        ab += w * texelFetch(s_sdfgi, texel + ivec3(3, 0, 0), 0).xyz;
        total += w;
    }
    if (total > 0.0) {
        a0 /= total;
        ar /= total;
        ag /= total;
        ab /= total;
    }
    return total;
}

// The probes' harmonics at p (unit normal n): the finest cascade holding it, blended into the next ones at its edge.
// Returns how much of the light is SDFGI's (1 well inside the cascades, falling to 0 past the last).
float kastSdfgiProbes(vec3 p, vec3 n, out vec3 a0, out vec3 ar, out vec3 ag, out vec3 ab)
{
    a0 = vec3_splat(0.0);
    ar = vec3_splat(0.0);
    ag = vec3_splat(0.0);
    ab = vec3_splat(0.0);
    float left = 1.0;
    for (int c = 0; c < 4; ++c) {
        if (float(c) >= u_sdfgi[4].w || left <= 0.0)
            break;
        float e = kastSdfgiEdge(c, p + n * (SDFGI_NORMAL_BIAS * u_sdfgi[c].w));
        if (e <= 0.0)
            continue;
        vec3 b0, br, bg, bb;
        if (kastSdfgiCascade(c, p, n, b0, br, bg, bb) <= 0.0)
            continue;
        float w = left * e;
        a0 += w * b0;
        ar += w * br;
        ag += w * bg;
        ab += w * bb;
        left -= w;
    }
    float used = 1.0 - left;
    if (used > 0.0) {
        a0 /= used;
        ar /= used;
        ag /= used;
        ab /= used;
    }
    return used;
}

// SDFGI's diffuse light at p (unit normal n), times its energy; fallback (the light without it) past the cascades.
vec3 kastSdfgiDiffuse(vec3 n, vec3 p, vec3 fallback)
{
    vec3 a0, ar, ag, ab;
    float w = kastSdfgiProbes(p, n, a0, ar, ag, ab);
    vec3 e = max(a0 + (2.0 / 3.0) * vec3(dot(ar, n), dot(ag, n), dot(ab, n)), vec3_splat(0.0)) * u_sdfgi[5].x;
    return mix(fallback, e, w);
}

// SDFGI's reflection on a rough surface: the probes' radiance along the reflection times the split sum's BRDF, over
// fallback (the environment's) from roughness SDFGI_ROUGHNESS - 0.1 to + 0.1.
vec3 kastSdfgiSpecular(vec3 n, vec3 v, vec3 p, float roughness, vec3 f0, float nv, vec3 fallback)
{
    float rough = smoothstep(SDFGI_ROUGHNESS - 0.1, SDFGI_ROUGHNESS + 0.1, roughness);
    if (rough <= 0.0)
        return fallback;
    vec3 r = reflect(-v, n);
    vec3 a0, ar, ag, ab;
    float w = kastSdfgiProbes(p, n, a0, ar, ag, ab) * rough;
    vec3 radiance = max(a0 + vec3(dot(ar, r), dot(ag, r), dot(ab, r)), vec3_splat(0.0)) * u_sdfgi[5].x;
    vec2 brdf = kastEnvBrdf(roughness, nv);
    return mix(fallback, radiance * (f0 * brdf.x + brdf.y), w);
}
#endif

// The diffuse GI selection point (u_gi.x, lightmap.h selectDiffuseGi): the irradiance / pi that replaces the
// environment's at unit normal n of the surface at p.
vec3 kastGiDiffuse(vec3 n, vec2 lightmapUv, vec3 p)
{
#ifdef KAST_HAS_LIGHTMAP
    if (u_gi.x < 1.5) {
        vec3 e = texture2D(s_lightmap, lightmapUv).rgb;
        if (u_gi.z > 0.5) { // lightmapDiffuse in lightmap.cpp
            vec4 d = texture2D(s_lightmapDir, lightmapUv);
            e *= (dot(n, d.xyz) * 0.5 + 0.5) / max(d.w, 0.25);
        }
        return e * u_gi.y;
    }
#endif
#ifdef KAST_HAS_SDFGI
    if (u_gi.x > 3.5) // SDFGI, over the probe grid or the environment the mesh gets without it
        return kastSdfgiDiffuse(n, p, u_sdfgi[5].y > 0.5 ? kastGiSh(n) : u_envSpecular.w > 0.0 ? kastSh9(n) : u_ambient.rgb);
#endif
#ifdef KAST_HAS_VOXELGI
    if (u_gi.x > 2.5) // VoxelGI, over the probe grid or the environment the mesh gets without it
        return kastVoxelDiffuse(n, p, u_voxelGi[3].y > 0.5 ? kastGiSh(n) : kastVoxelEnvironment(n));
#endif
    return kastGiSh(n);
}

void main()
{
    KastFragment f;
    f.uv = v_texcoord0 * u_uvTransform.xy + u_uvTransform.zw;
    vec4 base = u_baseColor * texture2D(s_baseColor, f.uv);
    vec4 mr = sampleMetallicRoughness(f.uv);
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
    f.normalMap = unpackNormalMap(texture2D(s_normal, f.uv));
    f.normalMapDepth = u_materialExt.x; // glTF normalTexture.scale
    // Ambient occlusion darkens ambient light only: ao = 1 + strength * (R - 1).
    f.ao = 1.0 + u_materialExt.y * (sampleOcclusion(f.uv).r - 1.0);
    f.screenUv = (gl_FragCoord.xy - u_viewRect.xy) * u_viewTexel.xy;
    fragment(f);

    if (f.alpha < u_material.z)
        discard;
    // Every return writes all five outputs: the lit color (0) and the G-data (1..4, see kastGNormal), which only the
    // opaque scene view of a PostProcess with SSAO or SSR has targets for (4 with SSR; elsewhere the writes are
    // discarded).
#ifdef KAST_SHADOW
    // The .kshader's shadow pass (fs_shadow.sc for the built-in material): depth only, cut out as the surface is.
    gl_FragData[0] = vec4_splat(0.0);
    gl_FragData[1] = vec4_splat(0.0);
    gl_FragData[2] = vec4_splat(0.0);
    gl_FragData[3] = vec4_splat(0.0);
    gl_FragData[4] = vec4_splat(0.0);
    return;
#endif
    float alpha = u_material.w > 0.5 ? f.alpha : 1.0;
    if (u_materialExt.w > 0.5) { // unlit: no indirect light
        gl_FragData[0] =
            vec4(kastVolumetricFog(fogged(f.albedo + f.emission, v_worldPos), v_worldPos, gl_FragCoord.xy), alpha);
        gl_FragData[1] = kastGNormal(f.normal, 1.0);
        gl_FragData[2] = vec4_splat(0.0);
        gl_FragData[3] = vec4_splat(0.0);
        gl_FragData[4] = vec4_splat(0.0);
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
#ifdef KAST_LIGHT
    // The .kshader's light() instead of shade(): once for the directional light, once per point, spot and area light
    // reaching the mesh (unused slots are black), accumulating DIFFUSE_LIGHT and SPECULAR_LIGHT.
    KastLight l;
    l.normal = n;
    l.view = v;
    l.albedo = albedo;
    l.metallic = metallic;
    l.roughness = roughness;
    l.diffuse = vec3_splat(0.0);
    l.specular = vec3_splat(0.0);
    if (u_lightDir.w > 0.5) {
        l.lightDirection = -u_lightDir.xyz;
        l.lightColor = u_lightColor.rgb;
        l.attenuation = receive && dot(u_lightColor.rgb, u_lightColor.rgb) > 0.0 ? csmShadow(v_worldPos, n) : 1.0;
        light(l);
    }
    for (int i = 0; i < MAX_LOCAL_LIGHTS; ++i) {
        if (dot(u_localColor[i].rgb, u_localColor[i].rgb) <= 0.0)
            continue;
        float type = u_localColor[i].w;
        float attenuation;
        if (type < LIGHT_AREA) {
            vec3 toLight = u_localPosRange[i].xyz - v_worldPos;
            float d = length(toLight);
            l.lightDirection = toLight / max(d, 1e-4);
            attenuation = falloff(d, u_localPosRange[i].w);
            if (type == LIGHT_SPOT)
                attenuation *= spotCone(dot(-l.lightDirection, u_localAxis[i].xyz), u_localShadow[i].z,
                                        u_localShadow[i].w);
            if (receive && u_localShadow[i].x >= 0.0 && attenuation > 0.0)
                attenuation *= type == LIGHT_SPOT ? spotShadow(i, v_worldPos, l.lightDirection, d, n)
                                                  : pointShadow(u_localShadow[i], -toLight, n);
        } else {
            // Area: toward the rect's closest point; the diffuse form factor over N.L there, so a Lambert light()
            // (N.L * ATTENUATION) gets about the LTC diffuse. ponytail: the specular is then a point light's from that
            // point, not the rect's reflection; light() would need the LTC terms for that.
            vec3 c0, c1, c2, c3;
            areaCorners(i, v_worldPos, c0, c1, c2, c3);
            float formFactor = ltcIntegrate(n, v, vec4(1.0, 0.0, 0.0, 1.0), c0, c1, c2, c3, type == LIGHT_AREA_TWO_SIDED);
            l.lightDirection = areaClosestDirection(i, v_worldPos);
            attenuation = areaVisibility(i, v_worldPos, n, receive) *
                          min(formFactor / max(dot(n, l.lightDirection), 0.01), 1.0);
        }
        l.lightColor = u_localColor[i].rgb;
        l.attenuation = attenuation;
        light(l);
    }
    vec3 color = l.diffuse + l.specular;
#else
    vec3 sun = u_lightColor.rgb * u_lightDir.w;
    if (receive && dot(sun, sun) > 0.0)
        sun *= csmShadow(v_worldPos, n);
    vec3 color = shade(n, v, -u_lightDir.xyz, sun, albedo, f0, metallic, roughness);
    for (int i = 0; i < MAX_LOCAL_LIGHTS; ++i) {
        float type = u_localColor[i].w;
        if (type < LIGHT_AREA) { // point and spot lights: shade(), as for the sun
            vec3 toLight = u_localPosRange[i].xyz - v_worldPos;
            float d = length(toLight);
            vec3 dir = toLight / max(d, 1e-4);
            vec3 light = u_localColor[i].rgb * falloff(d, u_localPosRange[i].w);
            if (type == LIGHT_SPOT)
                light *= spotCone(dot(-dir, u_localAxis[i].xyz), u_localShadow[i].z, u_localShadow[i].w);
            if (receive && u_localShadow[i].x >= 0.0 && dot(light, light) > 0.0)
                light *= type == LIGHT_SPOT ? spotShadow(i, v_worldPos, dir, d, n)
                                            : pointShadow(u_localShadow[i], -toLight, n);
            color += shade(n, v, dir, light, albedo, f0, metallic, roughness);
        } else if (dot(u_localColor[i].rgb, u_localColor[i].rgb) > 0.0) {
            vec3 light = u_localColor[i].rgb * areaVisibility(i, v_worldPos, n, receive);
            color += areaShade(i, v_worldPos, n, v, light, albedo, f0, metallic, roughness);
        }
    }
#endif

    // Ambient, weighted by the roughness-aware Schlick Fresnel F = F0 + (max(1 - roughness, F0) - F0) * (1 - N.V)^5, so
    // metals (no diffuse) still show it. With an environment (image-based lighting, ibl.h): its irradiance for the
    // diffuse, and the split sum for the specular, the prefiltered cube at the roughness's mip along the reflection
    // times the GGX BRDF's integral. Without: the flat ambient color for both.
    // With GI (u_gi.x > 0) the diffuse comes from kastGiDiffuse instead; GI off runs the code above unchanged.
    // indirectDiffuse / indirectSpecular: those terms of color as added (times f.ao), for the G-data; color itself is
    // summed exactly as before them.
    float nv = max(dot(n, v), 0.0);
    vec3 fAmbient = f0 + (max(vec3_splat(1.0 - roughness), f0) - f0) * pow(saturate(1.0 - nv), 5.0);
    vec3 indirectDiffuse, indirectSpecular;
    if (u_gi.x < 0.5) {
        if (u_envSpecular.w > 0.0) {
            vec3 r = kastEnvDirection(reflect(-v, n), v_worldPos);
            vec3 prefiltered = textureCubeLod(s_env, vec3(r.x, r.y, -r.z), roughness * u_envSpecular.w).rgb;
            vec2 brdf = kastEnvBrdf(roughness, nv);
            vec3 envDiffuse = (vec3_splat(1.0) - fAmbient) * (1.0 - metallic) * albedo * kastSh9(n);
            vec3 envSpecular = prefiltered * u_envSpecular.rgb * (f0 * brdf.x + brdf.y);
            color += (envDiffuse + envSpecular) * f.ao;
            indirectDiffuse = envDiffuse * f.ao;
            indirectSpecular = envSpecular * f.ao;
        } else {
            vec3 kd = (vec3_splat(1.0) - fAmbient) * (1.0 - metallic) * albedo;
            color += (kd + fAmbient) * u_ambient.rgb * f.ao;
            indirectDiffuse = kd * u_ambient.rgb * f.ao;
            indirectSpecular = fAmbient * u_ambient.rgb * f.ao;
        }
    } else {
        vec3 diffuse = (vec3_splat(1.0) - fAmbient) * (1.0 - metallic) * albedo * kastGiDiffuse(n, v_lightmapUv, v_worldPos);
        vec3 specular = fAmbient * u_ambient.rgb;
        if (u_envSpecular.w > 0.0) {
            vec3 r = kastEnvDirection(reflect(-v, n), v_worldPos);
            vec3 prefiltered = textureCubeLod(s_env, vec3(r.x, r.y, -r.z), roughness * u_envSpecular.w).rgb;
            vec2 brdf = kastEnvBrdf(roughness, nv);
            specular = prefiltered * u_envSpecular.rgb * (f0 * brdf.x + brdf.y);
        }
#ifdef KAST_HAS_VOXELGI
        if (u_voxelGi[3].w > 0.5) // in a VoxelGI volume: its reflection instead (lightmapped draws too)
            specular = kastVoxelSpecular(n, v, v_worldPos, roughness, f0, nv, specular);
#endif
#ifdef KAST_HAS_SDFGI
        if (u_sdfgi[5].z > 0.5) // a rough SDFGI mesh outside reflection probes: SDFGI's reflection instead
            specular = kastSdfgiSpecular(n, v, v_worldPos, roughness, f0, nv, specular);
#endif
        color += (diffuse + specular) * f.ao;
        indirectDiffuse = diffuse * f.ao;
        indirectSpecular = specular * f.ao;
    }

    color += f.emission;

    // Linear HDR: the post chain (post_process.h) tonemaps and encodes to sRGB.
    gl_FragData[0] = vec4(kastVolumetricFog(fogged(color, v_worldPos), v_worldPos, gl_FragCoord.xy), alpha);
    float keep = kastFogKeep(v_worldPos);
    gl_FragData[1] = kastGNormal(n, roughness);
    gl_FragData[2] = vec4(indirectDiffuse * keep, 0.0);
    gl_FragData[3] = vec4(indirectSpecular * keep, 0.0);
    // The split sum's specular weight, what SSR's reflected color is scaled by in place of the indirect specular.
    vec2 envBrdf = kastEnvBrdf(roughness, nv);
    gl_FragData[4] = vec4((f0 * envBrdf.x + envBrdf.y) * f.ao * keep, 0.0);
}
