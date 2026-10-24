#include <bgfx_compute.sh>

// VoxelGI inject (voxel_gi.h VoxelGiRenderer::update): one thread per baked voxel. Lights it with every light of
// u_voxelLights as fs_mesh.sc shades a rough surface (voxelGiLightIrradiance in voxel_gi.cpp is the same in C++), each
// occluded by this frame's shadow map of it (one hardware-compared tap; a point the directional light's cascades don't
// cover counts as lit), and writes albedo * that + emission, premultiplied by its opacity, as its direct light: into
// b_direct (for the bounces) and level 0 of the radiance volume.
BUFFER_RO(b_voxels, vec4, 0); // per voxel: (x, y, z, opacity), (albedo, 0), (normal, 0), (emission, 0)
BUFFER_RW(b_direct, vec4, 1); // per voxel: its direct light (rgb premultiplied, a: opacity)
IMAGE3D_WO(s_radiance, rgba16f, 2);
SAMPLER2DSHADOW(s_csm, 3);          // the shadow atlas as fs_mesh.sc's s_csm
SAMPLERCUBESHADOW(s_pointShadow0, 4); // the shadowed point light cubes as fs_mesh.sc's
SAMPLERCUBESHADOW(s_pointShadow1, 5);
SAMPLERCUBESHADOW(s_pointShadow2, 6);
SAMPLERCUBESHADOW(s_pointShadow3, 7);

#define MAX_LIGHTS 16           // kMaxVoxelGiLights in voxel_gi.h
#define MAX_CASCADES 4          // kMaxShadowCascades in scene.h
#define POINT_SHADOW_NEAR 0.05  // kPointShadowNear in shadow.h
#define PI 3.14159265

uniform vec4 u_voxel[4]; // [0] xyz: the grid's min corner, w: voxel size (m); [1] x: voxels, y: lights, z: cascades
uniform vec4 u_voxelLights[MAX_LIGHTS * 4]; // packVoxelGiLights in voxel_gi.h says how
uniform mat4 u_csmMtx[8];   // as fs_mesh.sc's
uniform vec4 u_csmBias[MAX_CASCADES];

// As fs_mesh.sc falloff (pointLightFalloff in renderer.cpp).
float falloff(float d, float range)
{
    float ratio = d / range;
    float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window / (d * d + 1.0);
}

// As fs_mesh.sc spotCone (spotConeFalloff in renderer.cpp).
float spotCone(float cosAngle, float cosInner, float cosOuter)
{
    float t = saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4));
    return t * t * (3.0 - 2.0 * t);
}

// As fs_mesh.sc edgeIntegral.
float edgeIntegral(vec3 a, vec3 b)
{
    a = a * inversesqrt(max(dot(a, a), 1e-12));
    b = b * inversesqrt(max(dot(b, b), 1e-12));
    float x = clamp(dot(a, b), -1.0, 1.0);
    float ratio = x > 0.99999 ? 1.0 : acos(x) * inversesqrt(max(1.0 - x * x, 1e-12));
    return cross(a, b).z * ratio;
}

// As fs_mesh.sc clippedEdge.
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

// fs_mesh.sc ltcIntegrate with the identity transform (rectFormFactor in renderer.cpp): the rect's diffuse form factor
// at unit normal n, corners relative to the surface.
float formFactor(vec3 n, vec3 c0, vec3 c1, vec3 c2, vec3 c3, bool twoSided)
{
    vec3 t1 = normalize(cross(abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 t2 = cross(n, t1);
    vec3 q0 = vec3(dot(t1, c0), dot(t2, c0), dot(n, c0));
    vec3 q1 = vec3(dot(t1, c1), dot(t2, c1), dot(n, c1));
    vec3 q2 = vec3(dot(t1, c2), dot(t2, c2), dot(n, c2));
    vec3 q3 = vec3(dot(t1, c3), dot(t2, c3), dot(n, c3));
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

// The directional light's shadow at p: the first cascade whose tile holds it (fs_mesh.sc csmShadow picks by view depth;
// any cascade covering the point holds its casters).
float sunShadow(vec3 p, vec3 n, float voxel)
{
    for (int c = 0; c < MAX_CASCADES; ++c) {
        if (float(c) >= u_voxel[1].z)
            break;
        vec3 coord = mul(u_csmMtx[c], vec4(p + n * (u_csmBias[c].x + voxel), 1.0)).xyz;
        float column = float(c - (c / 2) * 2);
        vec2 lo = vec2(0.5 * column, 0.25 * float(c / 2));
        vec2 hi = lo + vec2(0.5, 0.25);
        if (coord.x > lo.x && coord.y > lo.y && coord.x < hi.x && coord.y < hi.y && coord.z > 0.0 && coord.z < 1.0)
            return shadow2D(s_csm, vec3(coord.xy, coord.z - u_csmBias[c].y));
    }
    return 1.0;
}

// As fs_mesh.sc pointShadow, one tap: cube 0..3 seen from the light toward the voxel.
float pointShadow(int cube, vec3 toFragment, vec3 n, float range, float bias, float voxel)
{
    vec3 d = toFragment + n * voxel;
    vec3 a = abs(d);
    float m = max(max(a.x, a.y), a.z) - bias;
    float z = range / (range - POINT_SHADOW_NEAR) - range * POINT_SHADOW_NEAR / ((range - POINT_SHADOW_NEAR) * max(m, 1e-4));
    vec4 coord = vec4(d, z);
    if (cube == 0)
        return shadowCube(s_pointShadow0, coord);
    if (cube == 1)
        return shadowCube(s_pointShadow1, coord);
    if (cube == 2)
        return shadowCube(s_pointShadow2, coord);
    return shadowCube(s_pointShadow3, coord);
}

// As fs_mesh.sc tileShadow, one tap: a spot / area light's perspective tile (4..7); lit outside it.
float tileShadow(float tile, vec3 p, vec3 toLight, vec3 n, float bias, float voxel)
{
    vec4 q = mul(u_csmMtx[int(tile)], vec4(p + toLight * bias + n * voxel, 1.0));
    if (q.w <= 0.0)
        return 1.0;
    vec3 coord = q.xyz / q.w;
    float i = tile - float(MAX_CASCADES);
    vec2 lo = vec2(0.5 * (i - 2.0 * floor(i / 2.0)), 0.25 * (2.0 + floor(i / 2.0)));
    vec2 hi = lo + vec2(0.5, 0.25);
    if (coord.x < lo.x || coord.y < lo.y || coord.x > hi.x || coord.y > hi.y || coord.z >= 1.0)
        return 1.0;
    return shadow2D(s_csm, coord);
}

// Light i at p (unit normal n): its irradiance / pi there, shadowed.
vec3 lightAt(int i, vec3 p, vec3 n, float voxel)
{
    vec4 a = u_voxelLights[i * 4];
    vec4 b = u_voxelLights[i * 4 + 1];
    vec4 c = u_voxelLights[i * 4 + 2];
    vec4 d = u_voxelLights[i * 4 + 3];
    if (a.w > 3.5) { // directional: a.xyz the way it shines
        vec3 e = b.rgb * max(dot(n, -a.xyz), 0.0);
        if (d.z > 7.5 && dot(e, e) > 0.0)
            e *= sunShadow(p, n, voxel);
        return e;
    }
    vec3 toLight = a.xyz - p;
    float dist = length(toLight);
    vec3 l = toLight / max(dist, 1e-4);
    vec3 e;
    if (a.w < 1.5) { // point, spot
        e = b.rgb * (falloff(dist, b.w) * max(dot(n, l), 0.0));
        if (a.w > 0.5)
            e *= spotCone(dot(-l, c.xyz), d.x, c.w);
    } else { // area: its form factor times its range window (fs_mesh.sc areaVisibility)
        vec3 halfWidth = c.xyz;
        vec3 halfHeight = vec3(c.w, d.x, d.y);
        float ratio = dist / b.w;
        float window = saturate(1.0 - ratio * ratio * ratio * ratio);
        e = b.rgb * (window * window *
                     formFactor(n, toLight - halfWidth - halfHeight, toLight + halfWidth - halfHeight,
                                toLight + halfWidth + halfHeight, toLight - halfWidth + halfHeight, a.w > 2.5));
    }
    if (d.z >= 0.0 && dot(e, e) > 0.0)
        e *= d.z < 3.5 ? pointShadow(int(d.z), -toLight, n, b.w, d.w, voxel) : tileShadow(d.z, p, l, n, d.w, voxel);
    return e;
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (float(i) >= u_voxel[1].x)
        return;
    vec4 cell = b_voxels[i * 4u];
    vec3 albedo = b_voxels[i * 4u + 1u].xyz;
    vec3 n = b_voxels[i * 4u + 2u].xyz;
    vec3 emission = b_voxels[i * 4u + 3u].xyz;
    float voxel = u_voxel[0].w;
    vec3 p = u_voxel[0].xyz + (cell.xyz + 0.5) * voxel;
    vec3 e = vec3_splat(0.0);
    for (int l = 0; l < MAX_LIGHTS; ++l) {
        if (float(l) >= u_voxel[1].y)
            break;
        e += lightAt(l, p, n, voxel);
    }
    vec4 direct = vec4((albedo * e + emission) * cell.w, cell.w);
    b_direct[i] = direct;
    imageStore(s_radiance, ivec3(cell.xyz), direct);
}
