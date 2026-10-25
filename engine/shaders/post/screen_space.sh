// Shared by the screen-space passes (fs_ssao_*.sc, fs_ssr_*.sc; post_process.h PostProcess). Each view's projection is
// the camera's (PostProcess::setProjection), so u_proj / u_invProj map between the depth buffer and the view space the
// G-data's normals are in.

uniform vec4 u_depthParams; // x: 1 = uv (0, 0) is the bottom row (OpenGL); y: 1 = clip depth is -1..1
uniform vec4 u_texel;       // xy: 1 / the scene's size; zw: 1 / the half-resolution pass's size

#define KAST_PI 3.14159265
#define KAST_HALF_PI 1.57079633

// The view-space position under uv at depth (the depth buffer's 0..1).
vec3 kastViewPos(vec2 uv, float depth)
{
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, u_depthParams.x > 0.5 ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0);
    vec4 p = mul(u_invProj, vec4(ndc, u_depthParams.y > 0.5 ? depth * 2.0 - 1.0 : depth, 1.0));
    return p.xyz / p.w;
}

// G-data attachment 1's view-space normal (octDecode in post_process.cpp).
vec3 kastViewNormal(vec4 g)
{
    vec2 f = g.xy * 2.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// 4x4 ordered dither, 16 values k / 16: per-pixel rotation and jitter a 4x4 spatial filter averages out.
float kastBayer2(vec2 a)
{
    a = floor(a);
    return fract(a.x / 2.0 + a.y * a.y * 0.75);
}

float kastBayer4(vec2 a)
{
    return kastBayer2(0.5 * a) * 0.25 + kastBayer2(a);
}
