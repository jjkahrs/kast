#include <bgfx_compute.sh>

// Builds an environment cube for image-based lighting (ibl.h EnvironmentFilter), one thread per texel of one level of
// one cube (x, y, side), in three modes:
//   0 sky:        the scene's sky (the gradient, or its cube s_iblSource) into the source cube's level 0
//   1 downsample: the source cube (s_iblSource, its level 0) box-filtered into one level of the mip chain the prefilter
//                 samples: each texel the average of a block x block square of source texels
//   2 prefilter:  that chain (s_iblSource) GGX-filtered at one roughness into one level of the environment cube
// Every cube is in bgfx's side order (+X, -X, +Y, -Y, +Z, -Z) and space: a Kast world direction d is the cube direction
// (d.x, d.y, -d.z), as the sky (fs_sky.sc) and fs_mesh.sc sample them.
SAMPLERCUBE(s_iblSource, 0);
IMAGE2D_ARRAY_WO(s_iblTarget, rgba16f, 1);

uniform vec4 u_ibl[4];
// [0] x: mode, y: target level's size (texels per side), z: prefilter: roughness; downsample: block, w: source size
// [1] xyz: sky top color, w: 1 if the sky is s_iblSource (its cube), 0 the gradient
// [2] xyz: sky horizon color, w: prefilter: s_iblSource's mip count; sky: the lod its cube is sampled at
// [3] xyz: sky bottom color, w: prefilter: GGX samples per texel

#define PI 3.14159265

// The direction through (u, v) (-1..1 across the face, v down) of a cube side, as D3D lays cubes out. Not unit.
vec3 cubeDirection(uint side, vec2 uv)
{
    if (side == 0u)
        return vec3(1.0, -uv.y, -uv.x);
    if (side == 1u)
        return vec3(-1.0, -uv.y, uv.x);
    if (side == 2u)
        return vec3(uv.x, 1.0, uv.y);
    if (side == 3u)
        return vec3(uv.x, -1.0, -uv.y);
    if (side == 4u)
        return vec3(uv.x, -uv.y, 1.0);
    return vec3(-uv.x, -uv.y, -1.0);
}

// Van der Corput radical inverse: the Hammersley set's second coordinate.
float radicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Split-sum prefilter (Karis 2013) with filtered importance sampling (Colbert & Krivanek 2007): GGX half vectors around
// n (the view along n), each sample read from the mip whose texels cover its share of the lobe.
vec3 prefilter(vec3 n, float roughness)
{
    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n));
    vec3 ty = cross(n, tx);
    uint count = uint(u_ibl[3].w);
    float texelAngle = 4.0 * PI / (6.0 * u_ibl[0].w * u_ibl[0].w);
    float maxLod = u_ibl[2].w - 1.0;
    vec3 sum = vec3_splat(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < count; ++i) {
        float phi = 2.0 * PI * (float(i) + 0.5) / float(count);
        float xi = radicalInverse(i);
        float cosTheta = sqrt((1.0 - xi) / (1.0 + (a2 - 1.0) * xi));
        float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
        vec3 h = tx * (sinTheta * cos(phi)) + ty * (sinTheta * sin(phi)) + n * cosTheta;
        vec3 l = 2.0 * dot(n, h) * h - n;
        float nl = dot(n, l);
        if (nl <= 0.0)
            continue;
        float denom = cosTheta * cosTheta * (a2 - 1.0) + 1.0;
        float pdf = a2 / (PI * denom * denom) / 4.0; // D(N.H) N.H / (4 V.H), V = N
        float lod = clamp(0.5 * log2(1.0 / (float(count) * pdf * texelAngle)) + 1.0, 0.0, maxLod);
        sum += textureCubeLod(s_iblSource, l, lod).rgb * nl;
        weight += nl;
    }
    return sum / max(weight, 1e-6);
}

NUM_THREADS(8, 8, 1)
void main()
{
    uint size = uint(u_ibl[0].y);
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= size || id.y >= size)
        return;
    vec2 uv = (vec2(id.xy) + 0.5) / float(size) * 2.0 - 1.0;
    vec3 dir = cubeDirection(id.z, uv);
    vec3 color;
    if (u_ibl[0].x < 0.5) {
        if (u_ibl[1].w > 0.5) {
            color = textureCubeLod(s_iblSource, dir, u_ibl[2].w).rgb;
        } else { // proceduralSkyColor in sky.h, by the world direction's y (the cube's)
            float y = normalize(dir).y;
            color = mix(u_ibl[2].xyz, y >= 0.0 ? u_ibl[1].xyz : u_ibl[3].xyz, sqrt(abs(y)));
        }
    } else if (u_ibl[0].x < 1.5) {
        // Bilinear taps where four source texels meet each average them exactly: (block / 2)^2 taps (one, at the
        // texel's center, for a block of 1).
        float block = u_ibl[0].z;
        float source = u_ibl[0].w;
        int taps = max(int(block) / 2, 1);
        float step = block > 1.5 ? 2.0 : 1.0;
        float first = block > 1.5 ? 1.0 : 0.5;
        color = vec3_splat(0.0);
        for (int j = 0; j < taps; ++j)
            for (int i = 0; i < taps; ++i) {
                vec2 texel = vec2(id.xy) * block + vec2(first + step * float(i), first + step * float(j));
                color += textureCubeLod(s_iblSource, cubeDirection(id.z, texel / source * 2.0 - 1.0), 0.0).rgb;
            }
        color /= float(taps * taps);
    } else if (u_ibl[0].z <= 0.0) {
        color = textureCubeLod(s_iblSource, dir, 0.0).rgb; // roughness 0: the mirror image
    } else {
        color = prefilter(normalize(dir), u_ibl[0].z);
    }
    imageStore(s_iblTarget, ivec3(id), vec4(color, 1.0));
}
