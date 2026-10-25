$input v_texcoord0

#include <bgfx_shader.sh>
#include "ssao.sh"

SAMPLER2D(s_tex0, 0); // the scene's depth
SAMPLER2D(s_tex1, 1); // G-data normal + roughness

#define STEPS 6 // horizon samples per side of each slice

// The view-space position of the full-res depth texel under uv, at that texel's center: reconstructing at uv itself
// with a neighbor's depth would tilt flat surfaces into false occluders.
vec3 kastTexelPos(vec2 uv)
{
    uv = (floor(uv / u_texel.xy) + 0.5) * u_texel.xy;
    return kastViewPos(uv, texture2DLod(s_tex0, uv, 0.0).x);
}

// One slice's cosine-weighted visibility between horizons h0 <= 0 <= h1, n the projected normal's angle from the view
// vector (Jimenez et al. 2016 eq. 10; gtaoSliceVisibility in post_process.cpp).
float kastSliceVisibility(float n, float h0, float h1)
{
    float cosN = cos(n), sinN = sin(n);
    return 0.25 * (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) + 0.25 * (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - n));
}

// Ground-truth AO (GTAO) at half resolution: for u_ssao.y slices around the view vector, the highest horizon either
// side within u_ssao.x meters (samples fade to the open horizon over the radius's outer 61.5%, XeGTAO's falloff),
// integrated against the normal's cosine. Out: r visibility 0..1 (1 open), g distance from the eye
// (kastViewDistance; 0: no surface).
void main()
{
    vec2 uv = v_texcoord0;
    vec4 g = texture2DLod(s_tex1, uv, 0.0); // point sampled: the full-res texel kastTexelPos snaps uv to
    if (g.a < 0.5) {
        gl_FragColor = vec4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    uv = (floor(uv / u_texel.xy) + 0.5) * u_texel.xy;
    float depth = texture2DLod(s_tex0, uv, 0.0).x;
    vec3 p = kastViewPos(uv, depth);
    vec3 n = kastViewNormal(g);
    vec3 v = kastViewDir(p);
    float radius = u_ssao.x;
    vec4 c0 = mul(u_proj, vec4(p, 1.0));
    vec4 c1 = mul(u_proj, vec4(p + vec3(radius, 0.0, 0.0), 1.0));
    // ponytail: the search radius is capped at 64 half-res pixels (close-ups lose their widest occluders); a mip
    // pyramid of the depth would let it grow without the cache misses.
    float radiusPx = min(abs(c1.x / c1.w - c0.x / c0.w) * 0.5 / u_texel.z, 64.0);
    float falloffMul = -1.0 / (0.615 * radius);
    float falloffAdd = 0.385 / 0.615 + 1.0;
    vec2 pixel = floor(gl_FragCoord.xy);
    float rotation = kastBayer4(pixel);
    float jitter = kastBayer4(pixel.yx + vec2(1.0, 2.0));
    float slices = u_ssao.y;
    float visibility = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (float(i) >= slices)
            break;
        float phi = (float(i) + rotation) * KAST_PI / slices;
        vec2 dirUv = vec2(cos(phi), sin(phi)) * u_texel.zw; // one half-res pixel along the slice
        // The slice's direction in view space (towards + samples), at p's depth: no handedness or y-flip assumed.
        vec3 dir = kastViewPos(uv + dirUv, depth) - p;
        vec3 ortho = dir - v * dot(dir, v);
        vec3 axis = normalize(cross(dir, v));
        vec3 projN = n - axis * dot(n, axis);
        float projLen = length(projN);
        float cosN = saturate(dot(projN, v) / max(projLen, 1e-5));
        float nAngle = (dot(ortho, projN) >= 0.0 ? 1.0 : -1.0) * acos(cosN);
        float low1 = cos(nAngle + KAST_HALF_PI), low0 = cos(nAngle - KAST_HALF_PI);
        float h1Cos = low1, h0Cos = low0;
        for (int j = 0; j < STEPS; ++j) {
            float s = (float(j) + jitter) / float(STEPS);
            vec2 offset = dirUv * (s * s * radiusPx + 1.0);
            vec2 uv1 = uv + offset, uv0 = uv - offset;
            vec3 d1 = kastTexelPos(uv1) - p;
            vec3 d0 = kastTexelPos(uv0) - p;
            float l1 = length(d1), l0 = length(d0);
            h1Cos = max(h1Cos, mix(low1, dot(d1, v) / max(l1, 1e-5), saturate(l1 * falloffMul + falloffAdd)));
            h0Cos = max(h0Cos, mix(low0, dot(d0, v) / max(l0, 1e-5), saturate(l0 * falloffMul + falloffAdd)));
        }
        float h1 = acos(clamp(h1Cos, -1.0, 1.0)), h0 = -acos(clamp(h0Cos, -1.0, 1.0));
        h1 = nAngle + clamp(h1 - nAngle, -KAST_HALF_PI, KAST_HALF_PI);
        h0 = nAngle + clamp(h0 - nAngle, -KAST_HALF_PI, KAST_HALF_PI);
        visibility += projLen * kastSliceVisibility(nAngle, h0, h1);
    }
    gl_FragColor = vec4(saturate(visibility / slices), kastViewDistance(p), 0.0, 0.0);
}
