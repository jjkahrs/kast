$input v_texcoord0

#include <bgfx_shader.sh>
#include "screen_space.sh"

SAMPLER2D(s_tex0, 0); // the scene's depth
SAMPLER2D(s_tex1, 1); // G-data normal + roughness
SAMPLER2D(s_tex2, 2); // the opaque scene's HDR color (after SSAO)

uniform vec4 u_ssr; // x: max distance (m); y: thickness (m); z: max roughness; w: steps

#define MAX_STEPS 128
#define REFINE 5 // binary search steps after the linear march's first hit
#define EDGE 0.1 // the screen border (uv) over which hits fade out

// How far q is in front of the camera (view space's handedness either way: p's side is in front).
float kastAhead(vec3 q, float side)
{
    return q.z * side;
}

// The screen uv of a clip-space position.
vec2 kastScreenUv(vec4 clip)
{
    vec2 ndc = clip.xy / clip.w;
    return vec2(ndc.x * 0.5 + 0.5, u_depthParams.x > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5);
}

// How far behind the depth buffer the ray is at s (negative: in front), the ray from q0 / k0 to q1 / k1 (position / w,
// 1 / w: linear in screen space) over uv0 + delta * s.
float kastRayDelta(vec2 uv0, vec2 delta, vec3 q0, vec3 q1, float k0, float k1, float side, float s)
{
    vec2 uv = uv0 + delta * s;
    float rayDepth = kastAhead(mix(q0, q1, s) / mix(k0, k1, s), side);
    return rayDepth - kastAhead(kastViewPos(uv, texture2DLod(s_tex0, uv, 0.0).x), side);
}

// Screen-space reflections at half resolution: from each pixel's full-res texel, a reflection ray about a GGX sampled
// normal (roughness spreads the rays; the dither tile spreads the samples fs_ssr_composite.sc's gather averages) is
// marched linearly in screen space (perspective-correct depth) over u_ssr.w steps up to u_ssr.x meters; a step behind
// the depth buffer is refined by binary search, a hit if the crossing is less than u_ssr.y meters behind it. Out: rgb
// the scene's color at the hit times a, a the confidence 0..1 (0: miss): fading over the screen's edge, the ray's last 30%, the thickness, back
// faces and the last 25% of roughness below u_ssr.z.
void main()
{
    vec2 uv = (floor(v_texcoord0 / u_texel.xy) + 0.5) * u_texel.xy; // the full-res texel's center
    vec4 g = texture2DLod(s_tex1, uv, 0.0);
    float roughness = g.z;
    if (g.a < 0.5 || roughness >= u_ssr.z) {
        gl_FragColor = vec4_splat(0.0);
        return;
    }
    vec3 p = kastViewPos(uv, texture2DLod(s_tex0, uv, 0.0).x);
    vec3 n = kastViewNormal(g);
    vec3 v = kastViewDir(p);

    vec2 pixel = floor(gl_FragCoord.xy);
    vec2 xi = vec2(kastBayer4(pixel), kastBayer4(pixel.yx + vec2(1.0, 2.0))) + 1.0 / 32.0;
    float a2 = roughness * roughness * roughness * roughness; // GGX alpha = roughness^2
    float cosT = sqrt((1.0 - xi.x) / (1.0 + (a2 - 1.0) * xi.x));
    float sinT = sqrt(saturate(1.0 - cosT * cosT));
    float phi = 2.0 * KAST_PI * xi.y;
    vec3 tx = normalize(cross(abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 h = normalize(tx * (sinT * cos(phi)) + cross(n, tx) * (sinT * sin(phi)) + n * cosT);
    vec3 r = reflect(-v, h);
    if (dot(r, n) <= 0.0)
        r = reflect(-v, n); // sampled below the surface: the mirror ray

    // The ray's end: maxDistance along r, shortened to stay 5 cm in front of the camera.
    float side = p.z >= 0.0 ? 1.0 : -1.0;
    float len = u_ssr.x;
    float toward = kastAhead(r, side);
    if (toward < 0.0)
        len = min(len, (kastAhead(p, side) - 0.05) / -toward);
    vec3 end = p + r * len;
    vec4 c0 = mul(u_proj, vec4(p, 1.0));
    vec4 c1 = mul(u_proj, vec4(end, 1.0));
    vec2 uv0 = kastScreenUv(c0), uv1 = kastScreenUv(c1);
    // Clip the screen-space segment to the screen: s in 0..sMax.
    vec2 delta = uv1 - uv0;
    float sMax = 1.0;
    if (uv1.x > 1.0) sMax = min(sMax, (1.0 - uv0.x) / delta.x);
    if (uv1.x < 0.0) sMax = min(sMax, -uv0.x / delta.x);
    if (uv1.y > 1.0) sMax = min(sMax, (1.0 - uv0.y) / delta.y);
    if (uv1.y < 0.0) sMax = min(sMax, -uv0.y / delta.y);
    // Perspective-correct: position / w and 1 / w interpolate linearly in screen space.
    float k0 = 1.0 / c0.w, k1 = 1.0 / c1.w;
    vec3 q0 = p * k0, q1 = end * k1;

    float steps = u_ssr.w;
    float jitter = kastBayer4(pixel + vec2(2.0, 3.0));
    float prev = 0.0, s = 0.0, depthDelta = 0.0;
    bool hit = false;
    for (int i = 1; i <= MAX_STEPS; ++i) {
        if (float(i) > steps || hit)
            break;
        s = sMax * (float(i) - jitter) / steps;
        depthDelta = kastRayDelta(uv0, delta, q0, q1, k0, k1, side, s);
        if (depthDelta > 0.0) {
            // Crossed behind the depth buffer since prev: the crossing by binary search, a hit if it's within the
            // thickness (else the ray passed behind an edge: march on).
            float lo = prev, hi = s, d = depthDelta;
            for (int j = 0; j < REFINE; ++j) {
                float mid = 0.5 * (lo + hi);
                float dm = kastRayDelta(uv0, delta, q0, q1, k0, k1, side, mid);
                if (dm > 0.0) {
                    hi = mid;
                    d = dm;
                } else {
                    lo = mid;
                }
            }
            if (d < u_ssr.y) {
                hit = true;
                s = hi;
                depthDelta = d;
            }
        }
        prev = s;
    }
    if (!hit) {
        gl_FragColor = vec4_splat(0.0);
        return;
    }
    vec2 hitUv = uv0 + delta * s;
    vec4 hitG = texture2DLod(s_tex1, hitUv, 0.0);
    vec3 hitPos = mix(q0, q1, s) / mix(k0, k1, s);
    vec2 edge = min(hitUv, 1.0 - hitUv);
    float confidence = hitG.a < 0.5 ? 0.0 : 1.0;
    confidence *= smoothstep(0.0, EDGE, min(edge.x, edge.y));
    confidence *= 1.0 - smoothstep(0.7, 1.0, length(hitPos - p) / u_ssr.x);
    confidence *= saturate(1.0 - depthDelta / u_ssr.y);
    confidence *= saturate(-dot(kastViewNormal(hitG), r) * 4.0); // back faces and grazing hits
    confidence *= saturate((u_ssr.z - roughness) / (0.25 * u_ssr.z));
    gl_FragColor = vec4(texture2DLod(s_tex2, hitUv, 0.0).rgb * confidence, confidence);
}
