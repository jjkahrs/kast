// SDFGI checks for kast_self_check (docs/engine/sdfgi.html): cascade placement and the slabs a scroll rebuilds, toroidal
// cell addressing, the heightfield distance against brute force, the shapes' distances, the probe and cascade weights
// the shaders use, the presets and the settings' rules in scene files and the precedence. GPU-free.
#include "lightmap.h"
#include "log.h"
#include "scene.h"
#include "scene_io.h"
#include "sdfgi.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <source_location>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what, std::source_location where = std::source_location::current())
{
    if (ok)
        return;
    ++g_failures;
    kast::log::error(std::format("FAILED sdfgi_self_check.cpp line {}: {}", where.line(), what));
}

bool near(float a, float b, float epsilon) { return std::abs(a - b) <= epsilon; }
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void checkCascades()
{
    const bx::Vec3 eye{250.3f, 31.0f, 266.7f};
    for (float yScale : kast::kSdfgiYScales)
        for (int i = 0; i < kast::kMaxSdfgiCascades; ++i) {
            const kast::SdfgiCascade c = kast::sdfgiCascadeAt(eye, i, 0.5f, yScale);
            const bool snapped = std::ranges::all_of(c.origin, [](int o) { return o % kast::kSdfgiProbeCells == 0; });
            const bx::Vec3 lo = c.worldMin(), hi = c.worldMax();
            const bool holds = eye.x > lo.x && eye.y > lo.y && eye.z > lo.z && eye.x < hi.x && eye.y < hi.y && eye.z < hi.z;
            check(snapped && holds && c.cellSize == 0.5f * float(1 << i) &&
                      c.dims == std::array<int, 3>{64, int(64 * yScale), 64},
                  std::format("cascade {} (yScale {}): origin {} {} {} snapped to probes, holds the eye, cell {}", i,
                              yScale, c.origin[0], c.origin[1], c.origin[2], c.cellSize));
            // Near the middle: the eye is within a probe spacing of the box's center on every axis.
            const bx::Vec3 mid = bx::mul(bx::add(lo, hi), 0.5f);
            check(std::abs(mid.x - eye.x) <= c.probeSpacing() && std::abs(mid.z - eye.z) <= c.probeSpacing(),
                  "cascades are centered on the eye");
        }

    // Toroidal addressing: every cell of a cascade has its own texel, and a cell dims away shares it.
    const kast::SdfgiCascade c = kast::sdfgiCascadeAt({-37.0f, 5.0f, 12.0f}, 1, 0.5f, 0.5f);
    std::set<std::array<int, 3>> texels;
    for (int z = c.origin[2]; z < c.origin[2] + c.dims[2]; ++z)
        for (int y = c.origin[1]; y < c.origin[1] + c.dims[1]; ++y)
            for (int x = c.origin[0]; x < c.origin[0] + c.dims[0]; ++x)
                texels.insert(c.texel({x, y, z}));
    check(texels.size() == c.box().cells() && c.texel({-1, -65, 3}) == c.texel({63, -1, 67}) &&
              c.texel({-1, 0, 0}) == std::array<int, 3>{63, 0, 0},
          std::format("toroidal texels: {} distinct for {} cells", texels.size(), c.box().cells()));
}

// The cells of to not in from, brute force.
size_t exposed(const kast::SdfgiCascade& from, const kast::SdfgiCascade& to)
{
    const kast::CellBox overlap = kast::intersect(from.box(), to.box());
    return to.box().cells() - overlap.cells();
}

void checkScroll()
{
    const kast::SdfgiCascade at = kast::sdfgiCascadeAt({10.0f, 2.0f, -3.0f}, 0, 0.5f, 1.0f);
    check(kast::sdfgiScrollRegions(at, at).empty(), "a cascade that didn't move rebuilds nothing");
    // The camera moved by n cells along each axis and diagonally: exactly the exposed cells, in disjoint slabs inside the
    // new box and outside the old one.
    for (const auto& [dx, dy, dz] : {std::tuple(4, 0, 0), std::tuple(-8, 0, 0), std::tuple(0, 12, 0), std::tuple(0, 0, -4),
                                    std::tuple(4, -4, 8), std::tuple(-60, 4, 0), std::tuple(3, 0, 0), std::tuple(64, 0, 0),
                                    std::tuple(200, 0, 0)}) {
        const bx::Vec3 eye{10.0f + 0.5f * float(dx), 2.0f + 0.5f * float(dy), -3.0f + 0.5f * float(dz)};
        const kast::SdfgiCascade to = kast::sdfgiCascadeAt(eye, 0, 0.5f, 1.0f);
        const std::vector<kast::CellBox> regions = kast::sdfgiScrollRegions(at, to);
        size_t total = 0;
        bool inside = true, outsideOld = true, disjoint = true;
        for (size_t i = 0; i < regions.size(); ++i) {
            total += regions[i].cells();
            inside = inside && kast::intersect(regions[i], to.box()) == regions[i];
            outsideOld = outsideOld && kast::intersect(regions[i], at.box()).empty();
            for (size_t j = 0; j < i; ++j)
                disjoint = disjoint && kast::intersect(regions[i], regions[j]).empty();
        }
        const size_t slabs = size_t(std::abs(to.origin[0] - at.origin[0])) * 64 * 64; // along x alone
        check(total == exposed(at, to) && inside && outsideOld && disjoint && regions.size() <= 3 &&
                  (dy != 0 || dz != 0 || std::abs(dx) >= 64 || total == slabs),
              std::format("moving {} {} {} cells rebuilds the {} exposed cells (got {} in {} regions)", dx, dy, dz,
                          exposed(at, to), total, regions.size()));
    }
    // Moved past its extent (or another cell size): the whole box, once.
    const kast::SdfgiCascade far = kast::sdfgiCascadeAt({500.0f, 2.0f, -3.0f}, 0, 0.5f, 1.0f);
    check(kast::sdfgiScrollRegions(at, far) == std::vector<kast::CellBox>{far.box()} &&
              kast::sdfgiScrollRegions(at, kast::sdfgiCascadeAt({10.0f, 2.0f, -3.0f}, 0, 0.25f, 1.0f)).size() == 1,
          "a jump past the cascade rebuilds it in full");
    // Moving less than a probe spacing doesn't scroll at all.
    check(kast::sdfgiCascadeAt({10.4f, 2.0f, -3.0f}, 0, 0.5f, 1.0f) == at ||
              kast::sdfgiCascadeAt({9.6f, 2.0f, -3.0f}, 0, 0.5f, 1.0f) == at,
          "cascades scroll in whole probe slabs");

    const kast::CellBox near = kast::sdfgiCellsNear(at, {{12.0f, 1.0f, -4.0f}, {13.0f, 2.0f, -3.5f}}, 2.0f);
    check(near.min == std::array<int, 3>{22, 0, -10} && near.max == std::array<int, 3>{29, 7, -4},
          std::format("cells near a box: {} {} {} .. {} {} {}", near.min[0], near.min[1], near.min[2], near.max[0],
                      near.max[1], near.max[2]));
}

float hills(float x, float z) { return 3.0f * std::sin(0.15f * x) * std::cos(0.1f * z) + 0.05f * x + 20.0f; }

void checkHeightfield()
{
    // The approximation against the true distance to the surface (dense samples of it), near it: what the SDF's
    // clamped range (kSdfgiMaxDistance cells of 0.5 m) holds.
    float worst = 0.0f;
    bool signs = true;
    for (int i = 0; i < 40; ++i) {
        const float x = -20.0f + float(i * 7 % 40), z = 5.0f + float(i * 13 % 30), dy = -2.0f + float(i % 9) * 0.5f;
        const float y = hills(x, z) + dy;
        float brute = FLT_MAX;
        for (float sz = z - 3.0f; sz <= z + 3.0f; sz += 0.02f)
            for (float sx = x - 3.0f; sx <= x + 3.0f; sx += 0.02f) {
                const float h = hills(sx, sz);
                brute = std::min(brute, std::sqrt((sx - x) * (sx - x) + (h - y) * (h - y) + (sz - z) * (sz - z)));
            }
        brute = y >= hills(x, z) ? brute : -brute;
        const float e = 0.01f;
        const float gx = (hills(x + e, z) - hills(x - e, z)) / (2.0f * e), gz = (hills(x, z + e) - hills(x, z - e)) / (2.0f * e);
        const float approx = kast::sdfgiHeightfieldDistance(y, hills(x, z), std::sqrt(gx * gx + gz * gz));
        worst = std::max(worst, std::abs(approx - brute) - 0.15f * std::abs(brute));
        signs = signs && (approx >= 0.0f) == (brute >= 0.0f);
    }
    check(signs && worst <= 0.05f, std::format("heightfield distance within 0.05 m + 15% of brute force (off by {})", worst));
    check(near(kast::sdfgiHeightfieldDistance(3.0f, 1.0f, 0.0f), 2.0f, 1e-6f) &&
              near(kast::sdfgiHeightfieldDistance(1.0f, 0.0f, 1.0f), std::sqrt(0.5f), 1e-6f),
          "exact for planes");

    // A cascade's columns: the height at each column's center, its slope, in toroidal order; none where there's none.
    const kast::SdfgiCascade c = kast::sdfgiCascadeAt({0.0f, 20.0f, 20.0f}, 0, 0.5f, 1.0f);
    const auto columns = kast::sdfgiTerrainColumns(c, [](float x, float z) -> std::optional<float> {
        return x < 5.0f ? std::optional<float>(hills(x, z)) : std::nullopt;
    });
    bool heights = true;
    for (int z = c.origin[2]; z < c.origin[2] + c.dims[2]; z += 7)
        for (int x = c.origin[0]; x < c.origin[0] + c.dims[0]; x += 5) {
            const auto t = c.texel({x, 0, z});
            const auto& col = columns[size_t(t[2]) * 64 + size_t(t[0])];
            const float wx = (float(x) + 0.5f) * 0.5f, wz = (float(z) + 0.5f) * 0.5f;
            heights = heights && (wx < 4.5f ? near(col[0], hills(wx, wz), 1e-4f) && col[1] < 0.6f && col[1] > 0.0f
                                  : wx >= 5.0f ? col[0] == kast::kSdfgiNoTerrain
                                               : true);
        }
    check(heights && columns.size() == 64 * 64, "terrain columns: heights and slopes at each column, none off the map");
}

void checkShapes()
{
    float world[16];
    bx::mtxSRT(world, 2.0f, 4.0f, 1.0f, 0.0f, bx::toRad(90.0f), 0.0f, 10.0f, 0.0f, 0.0f);
    const kast::SdfgiShape box = kast::sdfgiShape(kast::SdfgiShapeType::Box, world, {0.5f, 0.5f, 0.5f}, {1, 0, 0}, {0, 0, 0});
    // Turned 90 degrees about y: its local x (2 m) runs along world z, its local z (1 m) along world x.
    check(near(kast::sdfgiShapeDistance(box, {10.0f, 0.0f, 0.0f}), -0.5f, 1e-5f) &&
              near(kast::sdfgiShapeDistance(box, {10.0f, 0.0f, 3.0f}), 2.0f, 1e-5f) &&
              near(kast::sdfgiShapeDistance(box, {12.0f, 0.0f, 0.0f}), 1.5f, 1e-5f) &&
              near(kast::sdfgiShapeDistance(box, {11.5f, 3.0f, 2.0f}), std::sqrt(3.0f), 1e-5f),
          "an oriented, scaled box's exact distance");
    check(near(box.bounds.min.x, 9.5f, 1e-5f) && near(box.bounds.max.z, 1.0f, 1e-5f) && near(box.bounds.max.y, 2.0f, 1e-5f),
          "its world bounds");
    bx::mtxSRT(world, 3.0f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f);
    const kast::SdfgiShape ball = kast::sdfgiShape(kast::SdfgiShapeType::Ellipsoid, world, {0.5f, 0.5f, 0.5f}, {0, 0, 0}, {0, 0, 0});
    check(near(kast::sdfgiShapeDistance(ball, {0.0f, 9.0f, 0.0f}), 2.5f, 1e-4f) &&
              near(kast::sdfgiShapeDistance(ball, {1.0f, 5.0f, 0.0f}), -0.5f, 1e-4f),
          "a sphere's exact distance");
    bx::mtxSRT(world, 4.0f, 1.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const kast::SdfgiShape plane = kast::sdfgiShape(kast::SdfgiShapeType::Box, world, {0.5f, 0.0f, 0.5f}, {0, 0, 0}, {0, 0, 0});
    check(near(kast::sdfgiShapeDistance(plane, {0.0f, 1.0f, 0.0f}), 1.0f - kast::kSdfgiPlaneHalf, 1e-5f),
          "a plane is a sliver");
    const kast::SdfgiShape model = kast::sdfgiBoxShape({{1.0f, 0.0f, 1.0f}, {3.0f, 2.0f, 5.0f}}, {0, 0, 0}, {0, 0, 0});
    check(near(kast::sdfgiShapeDistance(model, {4.0f, 1.0f, 3.0f}), 1.0f, 1e-5f) && model < box != box < model,
          "a model's bounds box; shapes order");
}

void checkWeights()
{
    const kast::SdfgiCascade c = kast::sdfgiCascadeAt({0.0f, 0.0f, 0.0f}, 0, 0.5f, 1.0f); // 2 m probes
    const auto all = [](int) { return true; };
    // A point in the middle of a probe cell, facing up (pushed half a meter up): the upper probes weigh more, each
    // layer's four alike, and they sum to 1.
    const kast::SdfgiProbeWeights up = kast::sdfgiProbeWeights(c, {1.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f}, all);
    float sum = 0.0f;
    for (float w : up.weights)
        sum += w;
    check(near(sum, 1.0f, 1e-5f) && up.base == std::array<int, 3>{0, 0, 0} && near(up.weights[0], up.weights[1], 1e-6f) &&
              near(up.weights[4], up.weights[5], 1e-6f) && up.weights[2] > up.weights[0] * 2.0f,
          std::format("probe weights sum to 1 ({}), probes above an upward surface weigh more", sum));
    // At a probe (after the bias), it alone counts but for the backface floor.
    const kast::SdfgiProbeWeights at = kast::sdfgiProbeWeights(c, {2.0f, 1.5f, 2.0f}, {0.0f, 1.0f, 0.0f}, all);
    check(at.base == std::array<int, 3>{1, 1, 1} && at.weights[0] > 0.999f, "a point on a probe takes that probe");
    // Invalid probes (inside geometry) drop out; none valid: all zero.
    const kast::SdfgiProbeWeights some = kast::sdfgiProbeWeights(c, {1.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f},
                                                                 [](int corner) { return corner != 2 && corner != 3; });
    const kast::SdfgiProbeWeights none = kast::sdfgiProbeWeights(c, {1.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f},
                                                                 [](int) { return false; });
    check(some.weights[2] == 0.0f && some.weights[3] == 0.0f && some.weights[6] > up.weights[6] &&
              std::ranges::all_of(none.weights, [](float w) { return w == 0.0f; }),
          "invalid probes are skipped, the others renormalized");

    // The cascade weight: 1 in the middle, falling to 0 at the outermost probes.
    const bx::Vec3 lo = c.worldMin();
    const float s = c.probeSpacing();
    check(near(kast::sdfgiCascadeWeight(c, bx::add(lo, bx::Vec3{16.0f, 16.0f, 16.0f})), 1.0f, 1e-6f) &&
              near(kast::sdfgiCascadeWeight(c, bx::add(lo, bx::Vec3{s, 16.0f, 16.0f})), 0.5f, 1e-5f) &&
              kast::sdfgiCascadeWeight(c, bx::add(lo, bx::Vec3{-1.0f, 16.0f, 16.0f})) == 0.0f &&
              kast::sdfgiCascadeWeight(c, bx::add(lo, bx::Vec3{15.0f * s, 16.0f, 16.0f})) == 0.0f,
          "a cascade fades out over its last two probes");
}

void checkSettings()
{
    const kast::SdfgiPreset low = kast::sdfgiPreset(kast::SdfgiQuality::Low),
                            medium = kast::sdfgiPreset(kast::SdfgiQuality::Medium),
                            high = kast::sdfgiPreset(kast::SdfgiQuality::High);
    check(low.rays < medium.rays && medium.rays < high.rays && low.steps < high.steps && medium.rays <= 64 && high.rays <= 64 &&
              high.steps <= 64,
          "presets grow from Low to High within the shaders' loops (64 rays, 64 steps)");

    check(kast::validateSdfgi({}) == nullptr, "the defaults are valid");
    for (const auto& [field, value, message] :
         {std::tuple("cascades", "0", "cascades"), std::tuple("cascades", "5", "cascades"),
          std::tuple("cascades", "2.5", "whole number"), std::tuple("cellSize", "0.1", "cellSize"),
          std::tuple("cellSize", "8", "cellSize"), std::tuple("energy", "-1", "energy"),
          std::tuple("bounceFeedback", "1.5", "bounceFeedback"), std::tuple("quality", "\"ultra\"", "quality"),
          std::tuple("yScale", "0.3", "yScale"), std::tuple("enabled", "1", "enabled"), std::tuple("bogus", "1", "unknown field")}) {
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json[field] = nlohmann::ordered_json::parse(value);
        kast::SceneSdfgi out;
        out.energy = 7.0f;
        std::string error;
        check(!kast::sdfgiFromJson(json, out, error) && contains(error, message) && out.energy == 7.0f,
              std::format("sdfgi {} = {} is rejected ({})", field, value, error));
    }
    kast::SceneSdfgi settings{.enabled = true, .cascades = 3, .cellSize = 0.25f, .energy = 1.5f, .bounceFeedback = 0.2f,
                              .quality = kast::SdfgiQuality::High, .yScale = 0.5f};
    kast::SceneSdfgi back;
    std::string error;
    check(kast::sdfgiFromJson(kast::sdfgiToJson(settings), back, error) && back == settings && !kast::validateSdfgi(back),
          "settings round trip " + error);

    // In a scene file: saved only when not default; a bad block loads as SDFGI off, with a warning.
    kast::Scene scene;
    check(!kast::sceneToJson(scene)["lighting"].contains("sdfgi"), "scenes without SDFGI save no sdfgi block");
    scene.lighting.sdfgi = settings;
    nlohmann::ordered_json json = kast::sceneToJson(scene);
    kast::Scene loaded;
    check(kast::sceneFromJson(json, loaded, error) && loaded.lighting.sdfgi == settings, "a scene keeps its SDFGI " + error);
    json["lighting"]["sdfgi"]["cellSize"] = 99;
    kast::SceneLoadReport report;
    check(kast::sceneFromJson(json, loaded, error, &report) && loaded.lighting.sdfgi == kast::SceneSdfgi{} &&
              report.warnings.size() == 1 && contains(report.warnings[0], "cellSize") &&
              contains(report.warnings[0], "SDFGI left off"),
          "out-of-range settings are dropped on load with a warning: " + (report.warnings.empty() ? "" : report.warnings[0]));

    check(kast::selectDiffuseGi(true, true, true, true) == kast::DiffuseGi::Lightmap &&
              kast::selectDiffuseGi(false, true, true, true) == kast::DiffuseGi::VoxelGi &&
              kast::selectDiffuseGi(false, true, false, true) == kast::DiffuseGi::Sdfgi &&
              kast::selectDiffuseGi(false, false, false, true) == kast::DiffuseGi::Sdfgi &&
              kast::selectDiffuseGi(false, true, false, false) == kast::DiffuseGi::ProbeGrid,
          "the diffuse precedence: lightmap > VoxelGI > SDFGI > probe grid > environment");
}

} // namespace

int sdfgiSelfCheck()
{
    g_failures = 0;
    checkCascades();
    checkScroll();
    checkHeightfield();
    checkShapes();
    checkWeights();
    checkSettings();
    kast::log::info(std::format("sdfgi_self_check: {} failure(s)", g_failures));
    return g_failures;
}
