// Global illumination checks for kast_self_check (docs/engine/lightmaps.html): the BVH against brute force, the path
// tracer against analytic furnaces, baked light falloffs against the renderer's, the probe grid's trilinear sample, the
// xatlas unwrap, the lightmap files' round trips and rejections, determinism and the scene settings. GPU-free.
#include "bvh.h"
#include "ibl.h"
#include "lightmap.h"
#include "lightmap_bake.h"
#include "log.h"
#include "model.h"
#include "renderer.h"
#include "scene.h"
#include "scene_io.h"
#include "shader.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <source_location>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what, std::source_location where = std::source_location::current())
{
    if (ok)
        return;
    ++g_failures;
    kast::log::error(std::format("FAILED lightmap_self_check.cpp line {}: {}", where.line(), what));
}

bool near(float a, float b, float epsilon) { return std::abs(a - b) <= epsilon; }
bool near(const bx::Vec3& a, const bx::Vec3& b, float epsilon)
{
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon) && near(a.z, b.z, epsilon);
}
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

kast::Mat4 identity()
{
    kast::Mat4 m{};
    bx::mtxIdentity(m.data());
    return m;
}

kast::Mat4 trs(const bx::Vec3& t, const bx::Vec3& s)
{
    kast::Mat4 m{};
    bx::mtxSRT(m.data(), s.x, s.y, s.z, 0.0f, 0.0f, 0.0f, t.x, t.y, t.z);
    return m;
}

// A two-triangle quad in the y = 0 plane facing +y, half extent h.
kast::BakeMesh quad(float h, uint32_t surface)
{
    kast::BakeMesh mesh;
    mesh.positions = {{-h, 0, -h}, {-h, 0, h}, {h, 0, h}, {h, 0, -h}};
    mesh.normals.assign(4, bx::Vec3{0, 1, 0});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.surface = surface;
    return mesh;
}

kast::BakeMesh primitive(kast::Primitive p, uint32_t surface)
{
    const kast::MeshData data = kast::primitiveMesh(p);
    kast::BakeMesh mesh;
    for (const kast::MeshVertex& v : data.vertices) {
        mesh.positions.push_back({v.x, v.y, v.z});
        mesh.normals.push_back({v.nx, v.ny, v.nz});
    }
    mesh.indices.assign(data.indices.begin(), data.indices.end());
    mesh.surface = surface;
    return mesh;
}

void checkBvh()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-10.0f, 10.0f), small(-1.5f, 1.5f);
    std::vector<kast::BvhTriangle> triangles;
    for (uint32_t i = 0; i < 400; ++i) {
        const bx::Vec3 c{u(rng), u(rng), u(rng)};
        triangles.push_back({bx::add(c, {small(rng), small(rng), small(rng)}), bx::add(c, {small(rng), small(rng), small(rng)}),
                             bx::add(c, {small(rng), small(rng), small(rng)}), i});
    }
    kast::Bvh bvh;
    bvh.build(triangles);
    int mismatches = 0, hits = 0;
    for (int r = 0; r < 4000; ++r) {
        const bx::Vec3 origin{u(rng) * 1.5f, u(rng) * 1.5f, u(rng) * 1.5f};
        const bx::Vec3 direction = bx::normalize(bx::Vec3{u(rng), u(rng), u(rng)});
        const float tMax = r % 3 == 0 ? 8.0f : 1e6f;
        const auto a = bvh.intersect(origin, direction, 0.0f, tMax);
        const auto b = kast::intersectBruteForce(triangles, origin, direction, 0.0f, tMax);
        hits += b ? 1 : 0;
        if (a.has_value() != b.has_value() || (a && (!near(a->t, b->t, 1e-4f) ||
                                                     (a->triangle != b->triangle && !near(a->t, b->t, 1e-6f)))))
            ++mismatches;
        if (bvh.occluded(origin, direction, 0.0f, tMax) != b.has_value())
            ++mismatches;
    }
    check(mismatches == 0 && hits > 200, std::format("BVH closest and any hit match brute force ({} mismatches, {} hits)",
                                                     mismatches, hits));
    kast::Bvh empty;
    empty.build({});
    check(!empty.intersect({0, 0, 0}, {0, 0, 1}, 0.0f, 1e6f) && !empty.occluded({0, 0, 0}, {0, 0, 1}, 0.0f, 1e6f),
          "an empty BVH hits nothing");
}

void checkFurnace()
{
    // A 4 km ground of albedo a under a uniform sky L: a texel on it facing up sees only sky (L); a point 1 m above it
    // facing down sees only ground lit by that sky (a L), after one bounce; none with bounces 0.
    kast::LightmapBakeInput input;
    input.surfaces = {{.albedo = {0.5f, 0.5f, 0.5f}}};
    input.sky.kind = kast::BakeSky::Kind::Uniform;
    input.sky.tint = {0.3f, 0.5f, 0.7f};
    input.entities.push_back({.id = 1, .source = "test", .world = identity(), .meshes = {quad(2000.0f, 0)}});
    {
        const kast::LightmapTracer tracer(input);
        kast::BakeRng rng(1, 1);
        const auto up = tracer.gather({0, 0, 0}, {0, 1, 0}, 256, 2, true, rng);
        const auto down = tracer.gather({0, 1, 0}, {0, -1, 0}, 1024, 2, true, rng);
        const auto none = tracer.gather({0, 1, 0}, {0, -1, 0}, 256, 0, true, rng);
        check(near(up.irradiance, {0.3f, 0.5f, 0.7f}, 1e-4f), "furnace: the open sky's irradiance / pi is its radiance");
        check(near(down.irradiance, {0.15f, 0.25f, 0.35f}, 0.01f),
              std::format("furnace: ground of albedo 0.5 under that sky, seen from above: a L (got {} {} {})",
                          down.irradiance.x, down.irradiance.y, down.irradiance.z));
        check(near(none.irradiance, {0, 0, 0}, 1e-6f) && up.backfaces == 0, "furnace: no bounce, no light from the ground");
        check(near(up.direction.y, 2.0f / 3.0f, 0.02f) && near(up.direction.x, 0.0f, 0.02f) && near(up.direction.z, 0.0f, 0.02f),
              "furnace: the mean direction under an open sky points up, cosine-weighted (2/3 of the way)");
    }
    // A closed, two-sided box emitting Le with albedo a, no sky: any point inside gathers Le (1 + a + ... + a^B) after B
    // bounces, exactly (every path hits a wall).
    kast::LightmapBakeInput box;
    box.surfaces = {{.albedo = {0.6f, 0.6f, 0.6f}, .emission = {1.0f, 1.0f, 1.0f}, .twoSided = true}};
    box.sky.tint = {0, 0, 0};
    box.entities.push_back({.id = 1, .source = "test", .world = trs({0, 0, 0}, {6, 6, 6}),
                            .meshes = {primitive(kast::Primitive::Cube, 0)}});
    const kast::LightmapTracer tracer(box);
    kast::BakeRng rng(2, 2);
    for (int bounces : {0, 1, 2, 4}) {
        const float expected = (1.0f - std::pow(0.6f, float(bounces + 1))) / (1.0f - 0.6f);
        const auto g = tracer.gather({0.3f, -0.2f, 0.1f}, bx::normalize(bx::Vec3{1, 1, 0}), 64, bounces, true, rng);
        check(near(g.irradiance, {expected, expected, expected}, 1e-3f),
              std::format("emissive furnace, {} bounces: Le (1 - a^(B+1)) / (1 - a) = {}, got {}", bounces, expected,
                          g.irradiance.x));
    }
}

void checkFalloffs()
{
    const bx::Vec3 p{0, 0, 0}, n{0, 1, 0};
    kast::BakeLight point{.sample = {.position = {1, 3, 0}, .range = 6, .color = {2, 1, 0.5f}}};
    const float d = std::sqrt(10.0f), nl = 3.0f / d;
    const float window = std::clamp(1.0f - std::pow(d / 6.0f, 4.0f), 0.0f, 1.0f);
    const float falloff = window * window / (d * d + 1.0f); // fs_mesh.sc falloff, written out
    check(near(kast::pointLightFalloff(d, 6.0f), falloff, 1e-6f), "pointLightFalloff: saturate(1 - (d/r)^4)^2 / (d^2 + 1)");
    check(near(kast::bakedLightIrradiance(point, p, n), bx::mul(bx::Vec3{2, 1, 0.5f}, falloff * nl), 1e-5f),
          "baked point light: color * pointLightFalloff * N.L, as the renderer");
    kast::BakeLight spot = point;
    spot.sample.type = kast::LocalLightType::Spot;
    spot.sample.direction = bx::normalize(bx::Vec3{-0.2f, -1.0f, 0.0f});
    spot.sample.cosInner = std::cos(bx::toRad(3.0f));
    spot.sample.cosOuter = std::cos(bx::toRad(30.0f));
    const float cone = kast::spotConeFalloff(bx::dot(bx::normalize(bx::Vec3{-1, -3, 0}), spot.sample.direction),
                                             spot.sample.cosInner, spot.sample.cosOuter);
    check(cone > 0.0f && cone < 1.0f && near(kast::bakedLightIrradiance(spot, p, n), bx::mul(bx::Vec3{2, 1, 0.5f}, falloff * nl * cone), 1e-5f),
          "baked spot light: times spotConeFalloff, as the renderer");
    kast::BakeLight area{.sample = {.position = {0, 2, 0}, .range = 5, .color = {1, 1, 1}, .type = kast::LocalLightType::Area,
                                    .direction = {0, -1, 0}, .halfWidth = {0.5f, 0, 0}, .halfHeight = {0, 0, -0.5f}}};
    const std::array<bx::Vec3, 4> corners = {bx::Vec3{-0.5f, 2, -0.5f}, {0.5f, 2, -0.5f}, {0.5f, 2, 0.5f}, {-0.5f, 2, 0.5f}};
    const float r = 2.0f / 5.0f, w = (1.0f - r * r * r * r) * (1.0f - r * r * r * r);
    const float form = kast::rectFormFactor(corners, n, false);
    const float formFlipped = kast::rectFormFactor({corners[3], corners[2], corners[1], corners[0]}, n, false);
    const float expected = std::max(form, formFlipped) * w; // whichever winding faces the point
    check(expected > 0.01f && near(kast::bakedLightIrradiance(area, p, n).x, expected, 1e-5f),
          "baked area light: rectFormFactor times the range window, as fs_mesh.sc areaShade");
    kast::BakeLight sun{.sample = {.color = {1, 1, 1}, .direction = bx::normalize(bx::Vec3{0, -1, -1})}, .directional = true};
    check(near(kast::bakedLightIrradiance(sun, p, n).x, std::sqrt(0.5f), 1e-5f), "baked directional light: N.L");
    check(!kast::lightShinesOn(kast::LightBakeMode::Full, true) && kast::lightShinesOn(kast::LightBakeMode::Full, false) &&
              kast::lightShinesOn(kast::LightBakeMode::Indirect, true) && kast::lightShinesOn(kast::LightBakeMode::Realtime, true),
          "a Full light skips lightmapped draws only; Indirect and Realtime lights shine on everything");
    check(kast::selectDiffuseGi(true, true) == kast::DiffuseGi::Lightmap &&
              kast::selectDiffuseGi(false, true) == kast::DiffuseGi::ProbeGrid &&
              kast::selectDiffuseGi(false, false) == kast::DiffuseGi::Environment,
          "diffuse GI precedence: lightmap, then probe grid, then environment");
    const bx::Vec3 dir = bx::normalize(bx::Vec3{0.3f, 0.8f, 0.1f});
    const std::array<float, 4> texel = {dir.x * 0.7f, dir.y * 0.7f, dir.z * 0.7f, bx::dot(n, bx::mul(dir, 0.7f)) * 0.5f + 0.5f};
    const bx::Vec3 e{0.4f, 0.5f, 0.6f};
    check(near(kast::lightmapDiffuse(e, texel, n, true), e, 1e-5f) && near(kast::lightmapDiffuse(e, texel, {1, 0, 0}, false), e, 1e-6f) &&
              kast::lightmapDiffuse(e, texel, dir, true).x > e.x && kast::lightmapDiffuse(e, texel, {0, -1, 0}, true).x < e.x,
          "directional lightmap: the stored value on the baked normal, more toward the light, less away");
}

void checkProbeGrid()
{
    kast::ProbeGrid grid{.origin = {0, 0, 0}, .spacing = 2.0f, .counts = {2, 2, 2}};
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                kast::Sh9 sh{};
                sh[0] = float(i) + 2.0f * float(j) + 4.0f * float(k); // c0 red: linear in x, y, z
                grid.sh.push_back(sh);
                grid.valid.push_back(1);
            }
    const auto red = [&](const bx::Vec3& p) { return kast::sampleProbeGrid(grid, p).value_or(kast::Sh9{})[0]; };
    check(near(red({1, 1, 1}), 3.5f, 1e-5f) && near(red({0.5f, 0, 0}), 0.25f, 1e-5f) && near(red({2, 2, 2}), 7.0f, 1e-5f),
          "probe grid: trilinear between the probes around a point");
    check(near(red({-5, 9, 1}), red({0, 2, 1}), 1e-6f), "probe grid: a point outside is clamped into it");
    grid.valid[7] = 0; // (1, 1, 1), value 7: buried in geometry
    // Next to it its weight goes to the valid corners (mostly 3, 5 and 6, its neighbours); right on it the seven valid
    // corners count evenly (0 + 1 + ... + 6) / 7 = 3. Never black (0), never the buried 7.
    const float beside = red({1.9f, 1.9f, 1.9f});
    check(beside > 3.0f && beside < 6.0f && near(red({2, 2, 2}), 3.0f, 1e-5f) && near(red({0, 0, 0}), 0.0f, 1e-6f) &&
              near(red({2, 2, 0}), 3.0f, 1e-5f),
          std::format("probe grid: an invalid probe is left out and the rest renormalized ({})", beside));
    std::ranges::fill(grid.valid, uint8_t(0));
    check(!kast::sampleProbeGrid(grid, {1, 1, 1}) && !kast::sampleProbeGrid(kast::ProbeGrid{}, {0, 0, 0}),
          "probe grid: no valid probe (or no grid) samples nothing, so the mesh falls back");
}

// Rasterizes an unwrap's triangles into its rect: no texel center inside two triangles (charts don't overlap) and every
// uv in 0..1.
bool unwrapValid(const kast::LightmapUnwrap& unwrap, std::string& why)
{
    std::vector<int> owner(size_t(unwrap.width) * unwrap.height, -1);
    int triangle = 0;
    for (const kast::LightmapSubmesh& sub : unwrap.submeshes) {
        if (sub.uv.size() != sub.xref.size() * 2 || sub.indices.size() % 3 != 0) {
            why = "sizes";
            return false;
        }
        for (float v : sub.uv)
            if (!(v >= 0.0f && v <= 1.0f)) {
                why = std::format("uv {} outside 0..1", v);
                return false;
            }
        for (size_t i = 0; i + 2 < sub.indices.size(); i += 3, ++triangle) {
            float x[3], y[3];
            for (int k = 0; k < 3; ++k) {
                x[k] = sub.uv[sub.indices[i + k] * 2] * unwrap.width;
                y[k] = sub.uv[sub.indices[i + k] * 2 + 1] * unwrap.height;
            }
            const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            if (std::abs(area) < 1e-9f)
                continue;
            for (int ty = 0; ty < unwrap.height; ++ty)
                for (int tx = 0; tx < unwrap.width; ++tx) {
                    const float cx = tx + 0.5f, cy = ty + 0.5f;
                    const float b0 = ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy)) / area;
                    const float b1 = ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy)) / area;
                    if (b0 <= 1e-3f || b1 <= 1e-3f || 1.0f - b0 - b1 <= 1e-3f)
                        continue; // strictly inside only: shared edges of neighbouring triangles are fine
                    int& o = owner[size_t(ty) * unwrap.width + tx];
                    if (o >= 0) {
                        why = std::format("triangles {} and {} overlap at texel {},{}", o, triangle, tx, ty);
                        return false;
                    }
                    o = triangle;
                }
        }
    }
    return true;
}

void checkUnwrap()
{
    for (const auto& [name, mesh, scale] :
         {std::tuple("cube", primitive(kast::Primitive::Cube, 0), bx::Vec3{1, 1, 1}),
          std::tuple("wall", primitive(kast::Primitive::Cube, 0), bx::Vec3{8, 4, 0.2f}),
          std::tuple("sphere", primitive(kast::Primitive::Sphere, 0), bx::Vec3{2, 2, 2})}) {
        kast::LightmapUnwrap unwrap;
        std::string error, why;
        const bool ok = kast::unwrapLightmapMeshes({mesh}, scale, 16.0f, unwrap, error);
        check(ok && unwrap.width > 0 && unwrap.height > 0 && unwrap.submeshes.size() == 1 &&
                  std::ranges::all_of(unwrap.submeshes[0].xref, [&](uint32_t x) { return x < mesh.positions.size(); }) &&
                  unwrap.submeshes[0].sourceVertices == mesh.positions.size() && unwrapValid(unwrap, why),
              std::format("xatlas unwrap of a {}: charts within 0..1, none overlapping ({}{})", name, error, why));
    }
    kast::LightmapUnwrap big, small;
    std::string error;
    kast::unwrapLightmapMeshes({primitive(kast::Primitive::Cube, 0)}, {1, 1, 1}, 32.0f, big, error);
    kast::unwrapLightmapMeshes({primitive(kast::Primitive::Cube, 0)}, {1, 1, 1}, 8.0f, small, error);
    check(big.width > small.width * 2, "unwrap: texels per meter set the rect's size");
    kast::BakeMesh given = quad(1.0f, 0);
    given.uv2 = {0, 0, 0, 1, 1, 1, 1, 0};
    kast::LightmapUnwrap kept;
    check(kast::unwrapLightmapMeshes({given}, {1, 1, 1}, 8.0f, kept, error) && kept.submeshes[0].uv == given.uv2 &&
              kept.submeshes[0].xref == std::vector<uint32_t>{0, 1, 2, 3} && kept.width == kept.height,
          "a mesh's own second UV set (glTF TEXCOORD_1) is kept as it is");
}

void checkFiles()
{
    kast::LightmapSidecar s{.hash = "0123456789abcdef"};
    s.atlases.push_back({.width = 64, .height = 32, .irradiance = "atlas0.ktx", .direction = "atlas0_dir.ktx",
                         .irradianceHash = 0x1234567890abcdefull, .directionHash = 42});
    s.meshesHash = 7;
    s.entities.push_back({.id = 12, .mesh = "primitive:cube@1,1,1@8", .atlas = 0, .scaleOffset = {0.5f, 0.25f, 0.125f, 0.0f}});
    s.grid = {.origin = {1, 2, 3}, .spacing = 0.5f, .counts = {2, 1, 1}};
    s.grid.sh.assign(2, kast::Sh9{});
    s.grid.sh[1][4] = 0.75f;
    s.grid.valid = {1, 0};
    const std::string json = kast::lightmapSidecarJson(s);
    kast::LightmapSidecar back;
    std::string error;
    check(kast::parseLightmapSidecar(json, back, error) && back.hash == s.hash && back.atlases.size() == 1 &&
              back.atlases[0].irradianceHash == s.atlases[0].irradianceHash && back.atlases[0].height == 32 &&
              back.entities.size() == 1 && back.entities[0].id == 12 && back.entities[0].scaleOffset == s.entities[0].scaleOffset &&
              back.grid.counts == s.grid.counts && back.grid.sh[1][4] == 0.75f && back.grid.valid == s.grid.valid &&
              near(back.grid.origin, {1, 2, 3}, 0.0f),
          "lightmaps.json round trip: " + error);
    const auto rejects = [&](const std::string& text, const std::string& expect, const char* what) {
        kast::LightmapSidecar ignored;
        std::string why;
        check(!kast::parseLightmapSidecar(text, ignored, why) && contains(why, expect),
              std::format("lightmaps.json rejects {} ('{}')", what, why));
    };
    const auto replaced = [&](const std::string& from, const std::string& to) {
        std::string text = json;
        const size_t at = text.find(from);
        check(at != std::string::npos, "the sidecar has " + from);
        return at == std::string::npos ? text : text.replace(at, from.size(), to);
    };
    rejects("not json", "not a lightmap sidecar", "text that isn't JSON");
    rejects(replaced("\"version\":1", "\"version\":2"), "version 2", "another version");
    rejects(replaced("\"atlas0.ktx\"", "\"../atlas0.ktx\""), "atlases", "a file outside its folder");
    rejects(replaced("\"irradianceHash\":\"1234567890abcdef\"", "\"irradianceHash\":7"), "atlases", "a malformed hash");
    rejects(replaced("\"atlas\":0", "\"atlas\":3"), "entities", "an entity on a missing atlas page");
    rejects(replaced("\"counts\":[2,1,1]", "\"counts\":[3,1,1]"), "probeGrid", "a probe count that doesn't match");
    rejects(replaced("\"spacing\":0.5", "\"spacing\":-1"), "probeGrid", "a negative probe spacing");

    std::vector<kast::LightmapMesh> meshes = {{"k", "primitive:cube", {{.sourceVertices = 4, .xref = {0, 1, 3}, .uv = {0, 0, 1, 0, 1, 1}, .indices = {0, 1, 2}}}}};
    const std::string bin = kast::writeLightmapMeshes(meshes);
    std::vector<kast::LightmapMesh> read;
    check(kast::readLightmapMeshes(bin, read, error) && read.size() == 1 && read[0].submeshes[0].xref == meshes[0].submeshes[0].xref &&
              read[0].submeshes[0].uv == meshes[0].submeshes[0].uv && read[0].source == "primitive:cube",
          "meshes.klmm round trip");
    check(!kast::readLightmapMeshes(bin.substr(0, bin.size() - 3), read, error) && contains(error, "cut short"),
          "meshes.klmm rejects a truncated file");
    std::string bad = bin;
    bad[bad.size() - 4] = 9; // an index past its vertices
    check(!kast::readLightmapMeshes(bad, read, error) && contains(error, "out of range"), "meshes.klmm rejects a bad index");

    kast::TerrainLightmaps index{.hash = "h", .size = 257, .tiles = {{1, -2, 3, 4}}};
    kast::TerrainLightmaps indexBack;
    check(kast::parseTerrainLightmaps(kast::terrainLightmapsJson(index), indexBack, error) && indexBack.size == 257 &&
              indexBack.tiles.size() == 1 && indexBack.tiles[0].tz == -2 && indexBack.tiles[0].directionHash == 4,
          "terrain lightmaps.json round trip");
    const std::vector<float> texels(4 * 4 * 4, 0.5f);
    const std::string ktx = kast::lightmapKtx(4, 4, texels);
    check(kast::checkLightmapKtx(ktx, 4, 4, error) && !kast::checkLightmapKtx(ktx, 8, 4, error) &&
              !kast::checkLightmapKtx("not a KTX", 4, 4, error),
          "lightmap KTX: RGBA16F of the expected size, and nothing else");
    check(kast::lightmapSidecarPath("scenes/room.scene") == "scenes/room_lightmaps/lightmaps.json" &&
              kast::terrainLightmapFile(3, -1, true) == "3_-1_dir.ktx",
          "lightmap paths beside the scene and in the map folder");
}

// A small scene's whole bake, twice: byte-identical files (each texel and probe its own random stream).
void checkDeterminism()
{
    kast::LightmapBakeInput input;
    input.settings.samplesPerTexel = 16;
    input.settings.probeSpacing = 2.0f;
    input.surfaces = {{.albedo = {0.7f, 0.2f, 0.2f}}, {.albedo = {0.8f, 0.8f, 0.8f}}};
    input.sky.tint = {0.2f, 0.3f, 0.4f};
    input.entities.push_back({.id = 1, .source = "primitive:plane", .world = trs({0, 0, 0}, {6, 1, 6}),
                              .meshes = {primitive(kast::Primitive::Plane, 1)}});
    input.entities.push_back({.id = 2, .source = "primitive:cube", .world = trs({0, 1, 0}, {1, 2, 1}),
                              .meshes = {primitive(kast::Primitive::Cube, 0)}});
    input.lights.push_back({.sample = {.position = {1, 3, 1}, .range = 8, .color = {2, 2, 2}}, .full = true});
    input.folder = "out";
    kast::LightmapBakeOutput a, b;
    kast::LightmapBakeProgress progress;
    std::string error;
    const bool baked = kast::bakeLightmaps(input, progress, a, error) && kast::bakeLightmaps(input, progress, b, error);
    bool same = baked && a.files.size() == b.files.size() && a.files.size() == 4;
    for (size_t i = 0; same && i < a.files.size(); ++i)
        same = a.files[i] == b.files[i];
    check(same && a.texels > 0 && a.probes > 0, "a bake is deterministic: the same files twice " + error);
    progress.cancel.store(true);
    kast::LightmapBakeOutput cancelled;
    check(!kast::bakeLightmaps(input, progress, cancelled, error) && error == "cancelled" && cancelled.files.empty(),
          "a cancelled bake returns nothing");
}

void checkSceneSettings()
{
    kast::Scene scene;
    const entt::entity box = scene.create("Box");
    scene.registry().emplace<kast::MeshRenderer>(box, kast::MeshRenderer{.giMode = kast::GiMode::Static});
    const entt::entity ball = scene.create("Ball");
    scene.registry().emplace<kast::MeshRenderer>(ball, kast::MeshRenderer{.giMode = kast::GiMode::Dynamic});
    const std::string before = kast::lightmapSceneHash(scene);
    scene.registry().get<kast::Transform>(ball).position.x += 1.0f;
    const std::string dynamicMoved = kast::lightmapSceneHash(scene);
    scene.registry().get<kast::Transform>(box).position.x += 1.0f;
    check(before == dynamicMoved && dynamicMoved != kast::lightmapSceneHash(scene),
          "the bake's hash: moving a Static mesh changes it, a Dynamic one doesn't");

    const nlohmann::ordered_json json = kast::sceneToJson(scene);
    check(!json["lighting"].contains("lightmaps") && json["entities"][0]["components"]["MeshRenderer"]["giMode"] == "static",
          "a scene without lightmap settings saves none; giMode saves by name");
    scene.lighting.lightmaps.bounces = 3;
    scene.lighting.lightmaps.bakedPath = "room_lightmaps/lightmaps.json";
    kast::Scene loaded;
    std::string error;
    check(kast::sceneFromJson(kast::sceneToJson(scene), loaded, error) && loaded.lighting.lightmaps == scene.lighting.lightmaps,
          "lighting.lightmaps round trip: " + error);
    for (const auto& [field, value, expect] :
         {std::tuple("bounces", "1.5", "whole number"), std::tuple("texelsPerMeter", "0", "between"),
          std::tuple("samplesPerTexel", "99999", "between"), std::tuple("enabled", "1", "true or false"),
          std::tuple("probeSpacing", "\"far\"", "between"), std::tuple("color", "1", "unknown field")}) {
        const std::string text = std::format(R"({{"version": 5, "entities": [], "lighting": {{"ambientColor": [1, 1, 1],
            "ambientIntensity": 1, "lightmaps": {{"{}": {}}}}}}})", field, value);
        kast::Scene rejected;
        check(!kast::sceneFromJson(nlohmann::ordered_json::parse(text), rejected, error) && contains(error, field) && contains(error, expect),
              std::format("lighting.lightmaps rejects {} {} ({})", field, value, error));
    }
    const nlohmann::ordered_json zeroScale = nlohmann::ordered_json::parse(R"({"version": 5, "entities": [{"id": 1, "name": "A", "parent": null,
        "components": {"Transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
                       "MeshRenderer": {"primitive": "cube", "material": "", "lightmapScale": 0}}}]})");
    kast::Scene rejected;
    check(!kast::sceneFromJson(zeroScale, rejected, error) && contains(error, "lightmapScale"), "lightmapScale must be > 0: " + error);
}

void checkModelUv2AndShaders()
{
    kast::Model model;
    model.nodes.push_back({"root", -1, {}});
    model.materials.push_back({"m", {}});
    kast::ModelMesh mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    mesh.uv2 = {0, 0, 1, 0, 0, 1};
    model.meshes.push_back(mesh);
    std::string error;
    const auto read = kast::readModel(kast::writeModel(model), error);
    check(read && read->meshes[0].uv2 == mesh.uv2, ".kmodel version 2 keeps a mesh's second UV set " + error);
    std::string v1 = kast::writeModel(model);
    // A version 1 file: no lods section (flags, 4 ratios, 4 thresholds, the mesh's 0 levels), no uv2 section.
    v1.resize(v1.size() - (4 + 4 + 16 + 4 + 16 + 4) - (4 + 6 * sizeof(float)));
    v1[4] = 1;
    const auto old = kast::readModel(v1, error);
    check(old && old->meshes[0].uv2.empty(), ".kmodel version 1 files still load (no second UV set) " + error);

    kast::ShaderSource source;
    check(kast::parseShader("param texture2D a;\nparam texture2D b;\nvoid fragment() {}\n", source, error) &&
              kast::shaderTakesLightmap(source) &&
              kast::parseShader("param texture2D a;\nparam texture2D b;\nparam texture2D c;\nvoid fragment() {}\n", source, error) &&
              !kast::shaderTakesLightmap(source) &&
              contains(kast::generateShader("//@user\n", source, kast::ShaderStage::Fragment), "#define KAST_TEXTURE_PARAMS 3"),
          ".kshaders with a third texture2D param take the lightmap's stages: their draws get the probe grid");

    // The baker's cube lookup is cubeTexelDirection's inverse.
    kast::CubeSides cube{.size = 4};
    for (int side = 0; side < 6; ++side)
        for (int i = 0; i < 16; ++i)
            cube.rgb[size_t(side)].insert(cube.rgb[size_t(side)].end(), {float(side), float(i), 0.0f});
    bool inverse = true;
    for (int side = 0; side < 6; ++side)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const bx::Vec3 d = kast::cubeTexelDirection(side, (x + 0.5f) / 2.0f - 1.0f, (y + 0.5f) / 2.0f - 1.0f);
                const bx::Vec3 c = kast::sampleCubeSides(cube, d);
                inverse = inverse && c.x == float(side) && c.y == float(y * 4 + x);
            }
    check(inverse, "sampleCubeSides finds the texel cubeTexelDirection points through");
}

} // namespace

int lightmapSelfCheck()
{
    g_failures = 0;
    checkBvh();
    checkFurnace();
    checkFalloffs();
    checkProbeGrid();
    checkUnwrap();
    checkFiles();
    checkDeterminism();
    checkSceneSettings();
    checkModelUv2AndShaders();
    kast::log::info(g_failures == 0 ? "All lightmap checks passed" : std::format("{} lightmap check(s) failed", g_failures));
    return g_failures;
}
