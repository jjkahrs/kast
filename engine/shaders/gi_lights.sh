// GI light passes' lights (cs_voxelgi_inject.sc, cs_sdfgi_light.sc): one light of packVoxelGiLights (voxel_gi.h) as
// fs_mesh.sc shades a rough surface with it, unshadowed (voxelGiLightIrradiance in voxel_gi.cpp is the same in C++).
#ifndef PI
#define PI 3.14159265
#endif

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

// The light packed as a, b, c, d (packVoxelGiLights) at p (unit normal n): its irradiance / pi there, unshadowed.
vec3 giLightIrradiance(vec4 a, vec4 b, vec4 c, vec4 d, vec3 p, vec3 n)
{
    if (a.w > 3.5) // directional: a.xyz the way it shines
        return b.rgb * max(dot(n, -a.xyz), 0.0);
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
    return e;
}
