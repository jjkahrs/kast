#include <bgfx_compute.sh>

// SDFGI build (sdfgi.h SdfgiRenderer::update): one thread per cell of a box of one cascade that needs it (scrolled in,
// or near a Static shape or terrain tile that came, went or moved). Its signed distance, in cells clamped to
// +-MAX_DISTANCE, to the nearest of the box's shapes (sdfgiShapeDistance in sdfgi.cpp) and the terrain under it
// (sdfgiHeightfieldDistance), with that surface's albedo and emission. A box scrolled in also marks its probes as
// having no history (probe texel 0's a = -1), so cs_sdfgi_probe takes their first rays whole.
BUFFER_RO(b_shapes, vec4, 0);     // 6 vec4 per shape: world to local rows (3), (half extents, type), albedo, emission
IMAGE2D_RO(s_terrain, rgba32f, 1); // per column: ground height, slope (sdfgiTerrainColumns), cascades stacked along y
IMAGE3D_WO(s_albedoOut, rgba8, 2);
IMAGE3D_WO(s_sdfOut, rgba16f, 3); // rgb emission, a distance (cells)
IMAGE3D_WO(s_probesOut, rgba16f, 4);

#define MAX_DISTANCE 4.0      // kSdfgiMaxDistance in sdfgi.h
#define NO_TERRAIN -100000.0  // below this: no terrain (kSdfgiNoTerrain)

uniform vec4 u_sdfgiCascades[4]; // as sdfgi.sh's
uniform vec4 u_sdfgiGrid;
// [0] xyz: the box's min (global cell), w: its cascade; [1] xyz: its size (cells), w: shapes; [2] x: the first shape,
// y: 1 to reset its probes, z: 1 if there is terrain; [3] rgb: the terrain's albedo
uniform vec4 u_sdfgiBuild[4];

vec3 wrap(vec3 x, vec3 m)
{
    return x - m * floor(x / m);
}

// sdfgiShapeDistance in sdfgi.cpp: a box (exact) or an ellipsoid (the usual bound) in its local frame.
float shapeDistance(vec3 q, vec3 h, float type)
{
    if (type < 0.5) {
        vec3 d = abs(q) - h;
        return length(max(d, vec3_splat(0.0))) + min(max(d.x, max(d.y, d.z)), 0.0);
    }
    float k0 = length(q / h);
    float k1 = length(q / (h * h));
    return k1 > 1e-6 ? k0 * (k0 - 1.0) / k1 : -min(h.x, min(h.y, h.z));
}

NUM_THREADS(4, 4, 4)
void main()
{
    vec3 size = u_sdfgiBuild[1].xyz;
    vec3 id = vec3(gl_GlobalInvocationID);
    if (id.x >= size.x || id.y >= size.y || id.z >= size.z)
        return;
    int c = int(u_sdfgiBuild[0].w);
    float cell = u_sdfgiCascades[c].w;
    vec3 g = u_sdfgiBuild[0].xyz + id;
    vec3 p = (g + 0.5) * cell;
    float best = MAX_DISTANCE * cell;
    vec3 albedo = vec3_splat(0.0);
    vec3 emission = vec3_splat(0.0);

    if (u_sdfgiBuild[2].z > 0.5) {
        vec3 t = wrap(g, u_sdfgiGrid.xyz);
        vec2 column = imageLoad(s_terrain, ivec2(int(t.x), int(t.z) + c * int(u_sdfgiGrid.z))).xy;
        if (column.x > NO_TERRAIN) {
            float d = (p.y - column.x) * inversesqrt(1.0 + column.y * column.y);
            if (d < best) {
                best = d;
                albedo = u_sdfgiBuild[3].rgb;
            }
        }
    }
    uint first = uint(u_sdfgiBuild[2].x);
    uint count = uint(u_sdfgiBuild[1].w);
    for (uint i = 0u; i < count; ++i) {
        uint s = (first + i) * 6u;
        vec4 r0 = b_shapes[s];
        vec4 r1 = b_shapes[s + 1u];
        vec4 r2 = b_shapes[s + 2u];
        vec4 h = b_shapes[s + 3u];
        vec3 q = vec3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
        float d = shapeDistance(q, h.xyz, h.w);
        if (d < best) {
            best = d;
            albedo = b_shapes[s + 4u].rgb;
            emission = b_shapes[s + 5u].rgb;
        }
    }

    ivec3 texel = ivec3(wrap(g, u_sdfgiGrid.xyz)) + ivec3(0, 0, c * int(u_sdfgiGrid.z));
    imageStore(s_albedoOut, texel, vec4(albedo, 1.0));
    imageStore(s_sdfOut, texel, vec4(emission, clamp(best / cell, -MAX_DISTANCE, MAX_DISTANCE)));
    // A probe sits on every PROBE_CELLS-th cell's min corner (sdfgi.h): its 4 texels along x in the probe texture.
    vec3 pq = g / 4.0;
    if (u_sdfgiBuild[2].y > 0.5 && all(equal(pq, floor(pq)))) {
        vec3 pd = u_sdfgiGrid.xyz / 4.0;
        vec3 pt = wrap(pq, pd);
        ivec3 base = ivec3(int(pt.x) * 4, int(pt.y), int(pt.z) + c * int(pd.z));
        imageStore(s_probesOut, base, vec4(0.0, 0.0, 0.0, -1.0));
        for (int k = 1; k < 4; ++k)
            imageStore(s_probesOut, base + ivec3(k, 0, 0), vec4_splat(0.0));
    }
}
