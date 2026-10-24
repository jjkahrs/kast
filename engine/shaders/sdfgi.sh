// SDFGI's compute passes (sdfgi.h SdfgiRenderer): the cascades and their toroidal textures. Every 3D texture stacks
// the cascades along z (cascade c at z c * dims.z); a global cell g of cascade c lives at texel g mod dims there.
#define MAX_CASCADES 4         // kMaxSdfgiCascades in scene.h
#define PROBE_CELLS 4          // kSdfgiProbeCells in sdfgi.h
#define NORMAL_BIAS 0.25       // kSdfgiNormalBias in sdfgi.h

uniform vec4 u_sdfgiCascades[MAX_CASCADES]; // per cascade: xyz its origin (a global cell), w its cell size (m)
uniform vec4 u_sdfgiGrid;                   // xyz: cells per axis of a cascade, w: cascades

// x mod m, non-negative, for whole numbers held in floats.
vec3 sdfgiWrap(vec3 x, vec3 m)
{
    return x - m * floor(x / m);
}

// The stacked texel of global cell g of cascade c.
ivec3 sdfgiTexel(vec3 g, int c)
{
    vec3 t = sdfgiWrap(g, u_sdfgiGrid.xyz);
    return ivec3(t) + ivec3(0, 0, c * int(u_sdfgiGrid.z));
}

// The global cell (whole floats) of stacked texel t (its cascade c), as sdfgi.h SdfgiCascade::texel inverts.
vec3 sdfgiCellOf(ivec3 t, int c)
{
    vec3 o = u_sdfgiCascades[c].xyz;
    vec3 local = vec3(t) - vec3(0.0, 0.0, float(c) * u_sdfgiGrid.z);
    return o + sdfgiWrap(local - o, u_sdfgiGrid.xyz);
}

// Whether p (world) is in cascade c with half a cell to spare.
bool sdfgiInside(vec3 p, int c)
{
    vec3 local = p / u_sdfgiCascades[c].w - u_sdfgiCascades[c].xyz;
    return all(greaterThanEqual(local, vec3_splat(0.5))) && all(lessThan(local, u_sdfgiGrid.xyz - vec3_splat(0.5)));
}

// Sphere-traces the SDF (s_sdf: a in cells) from o along unit d, from tMin (past the surface it starts on) to tMax
// meters, in cascade c and on into the coarser ones it leaves, at most steps samples. A cell under 0.6 cells from a
// surface is a hit: hit its stacked texel, returns true. Leaving the last cascade, running out of steps or reaching
// tMax: false.
bool sdfgiMarch(vec3 o, vec3 d, float tMin, float tMax, float steps, int c, out ivec3 hit)
{
    hit = ivec3(0, 0, 0);
    float t = tMin;
    for (int i = 0; i < 64; ++i) {
        if (float(i) >= steps || t > tMax)
            return false;
        vec3 p = o + d * t;
        for (int k = 0; k < MAX_CASCADES; ++k) {
            if (c >= int(u_sdfgiGrid.w) || sdfgiInside(p, c))
                break;
            ++c;
        }
        if (c >= int(u_sdfgiGrid.w))
            return false;
        float cell = u_sdfgiCascades[c].w;
        ivec3 texel = sdfgiTexel(floor(p / cell), c);
        float dc = imageLoad(s_sdf, texel).a;
        if (dc < 0.6) {
            hit = texel;
            return true;
        }
        t += max(dc - 0.5, 0.5) * cell;
    }
    return false;
}
