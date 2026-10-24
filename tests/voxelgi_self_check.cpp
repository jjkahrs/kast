// VoxelGI checks for kast_self_check (docs/engine/voxelgi.html): the voxel grid's addressing, the volume selection rule
// and its edge fade, the cone sets and steps the shaders march, the injected lights' falloffs against the renderer's,
// the voxel and sidecar files' round trips and rejections, the voxelizer and the bake's hash, and the component's
// scene file rules. GPU-free.
#include "lightmap.h"
#include "lightmap_bake.h"
#include "log.h"
#include "renderer.h"
#include "scene.h"
#include "scene_io.h"
#include "shader.h"
#include "voxel_gi.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <format>
#include <source_location>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what, std::source_location where = std::source_location::current())
{
    if (ok)
        return;
    ++g_failures;
    kast::log::error(std::format("FAILED voxelgi_self_check.cpp line {}: {}", where.line(), what));
}

bool near(float a, float b, float epsilon) { return std::abs(a - b) <= epsilon; }
bool near(const bx::Vec3& a, const bx::Vec3& b, float epsilon)
{
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon) && near(a.z, b.z, epsilon);
}
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void checkGrid()
{
    // voxelgi_room's box: 128 voxels along x and z, as many as cover y, centered on the box.
    const kast::VoxelGrid grid = kast::voxelGridFor({0.0f, 2.0f, -6.0f}, {6.6f, 4.6f, 6.6f}, 128);
    check(grid.dims == std::array<int, 3>{128, 90, 128} && near(grid.voxelSize, 6.6f / 128.0f, 1e-6f),
          std::format("grid dims {}x{}x{}", grid.dims[0], grid.dims[1], grid.dims[2]));
    const bx::Vec3 extent = grid.extent();
    check(extent.y >= 4.6f - 1e-4f && extent.y < 4.6f + grid.voxelSize && near(grid.center(), {0.0f, 2.0f, -6.0f}, 1e-4f),
          "the grid covers the box in whole voxels, centered on it");
    check(kast::voxelGridMips(grid) == 8 && kast::voxelGridMips(kast::voxelGridFor({0, 0, 0}, {1, 1, 1}, 64)) == 7,
          "mips down to one voxel along the longest axis");
    check(kast::voxelGridFor({0, 0, 0}, {10.0f, 0.001f, 10.0f}, 64).dims[1] == 1, "a flat box still has one voxel layer");

    bool roundTrip = true, centers = true;
    for (size_t index = 0; index < grid.count(); index += 9973) {
        const auto c = grid.coord(index);
        roundTrip = roundTrip && grid.index(c[0], c[1], c[2]) == index;
        const auto at = grid.voxelAt(grid.voxelCenter(c[0], c[1], c[2]));
        centers = centers && at && *at == c;
    }
    check(roundTrip && centers, "index <-> coord, and each voxel's center lies in it");
    check(grid.index(1, 0, 0) == 1 && grid.index(0, 1, 0) == 128 && grid.index(0, 0, 1) == 128 * 90,
          "x fastest, then y, then z");
    const bx::Vec3 hi = bx::add(grid.origin, extent);
    check(!grid.voxelAt(bx::sub(grid.origin, {0.001f, 0.0f, 0.0f})) && !grid.voxelAt(hi) && grid.voxelAt(grid.origin) &&
              *grid.voxelAt(grid.origin) == std::array<int, 3>{0, 0, 0},
          "voxelAt: the min faces are in, the max faces and outside are out");
}

void checkSelection()
{
    const kast::VoxelGrid room = kast::voxelGridFor({0, 2, -6}, {6, 4, 6}, 64);
    const kast::VoxelGrid alcove = kast::voxelGridFor({2.5f, 2, -6}, {2, 4, 2}, 64);
    std::vector<kast::ProbeBox> boxes{kast::voxelGiBox(room, 0), kast::voxelGiBox(alcove, 0)};
    check(kast::selectProbe(boxes, {-2, 1, -6}) == 0u, "a point only in the room's box selects it");
    check(kast::selectProbe(boxes, {2.4f, 1, -6}) == 1u, "equal priority: the nearest box center wins");
    boxes[0].priority = 1;
    check(kast::selectProbe(boxes, {2.4f, 1, -6}) == 0u, "the higher priority wins over the nearer center");
    check(!kast::selectProbe(boxes, {0, 9, -6}), "outside every box: none");

    const float voxel = room.voxelSize;
    const float margin = float(kast::kVoxelGiEdgeVoxels) * voxel;
    const bx::Vec3 lo = room.origin;
    check(near(kast::voxelGiEdgeWeight(room, room.center()), 1.0f, 1e-6f) &&
              near(kast::voxelGiEdgeWeight(room, {lo.x, 2, -6}), 0.0f, 1e-6f) &&
              near(kast::voxelGiEdgeWeight(room, {lo.x + 0.5f * margin, 2, -6}), 0.5f, 1e-4f) &&
              near(kast::voxelGiEdgeWeight(room, {lo.x + margin, 2, -6}), 1.0f, 1e-4f) &&
              kast::voxelGiEdgeWeight(room, {lo.x - 1.0f, 2, -6}) == 0.0f,
          "the edge fade: 0 at the faces and outside, 1 from kVoxelGiEdgeVoxels voxels in");
}

void checkCones()
{
    for (const kast::VoxelGiQuality quality : {kast::VoxelGiQuality::Low, kast::VoxelGiQuality::Medium, kast::VoxelGiQuality::High}) {
        const kast::VoxelGiPreset preset = kast::voxelGiPreset(quality);
        const auto cones = kast::voxelGiCones(preset.cones);
        float weights = 0.0f;
        bool unit = true;
        for (const auto& c : cones) {
            weights += c[3];
            unit = unit && near(std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), 1.0f, 1e-5f) && c[2] > 0.0f;
        }
        check(int(cones.size()) == preset.cones && preset.cones <= kast::kMaxVoxelGiCones && near(weights, 1.0f, 1e-5f) && unit,
              std::format("{} cones: unit, in the hemisphere, weights summing to 1 (a uniform radiance gathers itself)",
                          preset.cones));
        check(cones[0] == std::array<float, 4>{0.0f, 0.0f, 1.0f, cones[0][3]} && cones[0][3] > cones.back()[3],
              "the first cone along the normal, weighted most");
    }
    const kast::VoxelGiPreset low = kast::voxelGiPreset(kast::VoxelGiQuality::Low);
    const kast::VoxelGiPreset medium = kast::voxelGiPreset(kast::VoxelGiQuality::Medium);
    const kast::VoxelGiPreset high = kast::voxelGiPreset(kast::VoxelGiQuality::High);
    check(low.cones < medium.cones && medium.cones < high.cones && low.stepScale > high.stepScale &&
              low.specularSteps < high.specularSteps && medium.bounces >= 1 && high.bounces > medium.bounces,
          "the presets scale cones, steps and bounces");

    const float voxel = 0.05f;
    const auto steps = kast::voxelConeSteps(medium.aperture, voxel, medium.stepScale, 6.0f, 64);
    bool ok = !steps.empty() && near(steps[0].distance, voxel, 1e-6f);
    for (size_t i = 0; i < steps.size(); ++i) {
        const kast::ConeStep& s = steps[i];
        ok = ok && near(s.diameter, std::max(voxel, 2.0f * medium.aperture * s.distance), 1e-5f) &&
             near(s.lod, std::log2(s.diameter / voxel), 1e-4f) && s.distance < 6.0f;
        if (i > 0)
            ok = ok && near(s.distance, steps[i - 1].distance + steps[i - 1].diameter * medium.stepScale, 1e-4f);
    }
    check(ok && steps.size() < 16, std::format("diffuse cone steps: a voxel out, then diameter * step scale ({} steps)",
                                               steps.size()));
    const auto mirror = kast::voxelConeSteps(0.0f, voxel, 0.5f, 100.0f, 10);
    check(mirror.size() == 10 && near(mirror[9].distance, voxel + 9 * 0.5f * voxel, 1e-5f) && mirror[9].lod == 0.0f,
          "a zero-aperture cone steps by a voxel's diameter at mip 0, at most maxSteps");
    const auto specular = kast::voxelConeSteps(kast::voxelGiSpecularAperture(0.05f), voxel, medium.stepScale, 3.0f, 1000);
    check(specular.size() <= size_t(medium.specularSteps),
          std::format("a mirror cone reaches 3 m within Medium's specular steps ({})", specular.size()));
    check(kast::voxelGiSpecularAperture(0.0f) == kast::kVoxelGiMinAperture && kast::voxelGiSpecularAperture(1.0f) == 1.0f &&
              kast::voxelGiSpecularAperture(0.5f) > kast::voxelGiSpecularAperture(0.3f),
          "the specular aperture widens with roughness, from kVoxelGiMinAperture");
}

void checkLights()
{
    const kast::Aabb box{{-3, 0, -9}, {3, 4, -3}};
    std::vector<kast::LocalLightSample> lights;
    lights.push_back({.position = {1, 3, -5}, .range = 8, .color = {1, 0.8f, 0.6f}, .castShadows = true, .bias = 0.05f});
    lights.push_back({.position = {-2, 3.5f, -6}, .range = 10, .color = {0.5f, 0.6f, 1}, .type = kast::LocalLightType::Spot,
                      .direction = bx::normalize(bx::Vec3{0.3f, -1, 0.1f}), .cosInner = 0.8f, .cosOuter = 0.6f});
    lights.push_back({.position = {0, 3.9f, -6}, .range = 6, .color = {3, 3, 3}, .type = kast::LocalLightType::Area,
                      .direction = {0, -1, 0}, .halfWidth = {0.5f, 0, 0}, .halfHeight = {0, 0, 0.4f}, .twoSided = false});
    lights.push_back({.position = {50, 3, -5}, .range = 5, .color = {1, 1, 1}}); // out of reach
    const std::vector<float> shadows{0.0f, 4.0f, -1.0f, -1.0f};
    const bx::Vec3 sunDir = bx::normalize(bx::Vec3{0.2f, -1, -0.3f});
    const kast::VoxelGiLights packed = kast::packVoxelGiLights(&sunDir, {0.9f, 0.9f, 0.8f}, true, lights, shadows, box);
    check(packed.count == 4, std::format("the sun and the three lights reaching the box ({})", packed.count));
    check(packed.data[0][3] == float(kast::VoxelGiLightType::Directional) && packed.data[3][2] == 8.0f,
          "the directional light first, its shadow the cascades (8)");

    bool parity = true;
    std::string where;
    const bx::Vec3 points[] = {{0, 0, -5}, {-2.5f, 1, -7}, {1.5f, 2, -4}, {0, 3.5f, -6}, {2.9f, 0.2f, -8.5f}};
    const bx::Vec3 normals[] = {{0, 1, 0}, {1, 0, 0}, bx::normalize(bx::Vec3{-1, 1, 1}), {0, 1, 0}, {0, 0, 1}};
    for (size_t p = 0; p < std::size(points); ++p) {
        const bx::Vec3 sun = kast::voxelGiLightIrradiance(packed, 0, points[p], normals[p]);
        parity = parity && near(sun, bx::mul(bx::Vec3{0.9f, 0.9f, 0.8f}, std::max(bx::dot(normals[p], bx::neg(sunDir)), 0.0f)), 1e-5f);
        // Packed in nearest-first order: point (1 from the center), spot, area.
        const kast::LocalLightSample* order[] = {&lights[0], &lights[2], &lights[1]};
        for (size_t i = 0; i < 3; ++i) {
            const kast::LocalLightSample& light = *order[i];
            size_t slot = 0;
            for (size_t s = 1; s < packed.count; ++s)
                if (packed.data[s * 4][0] == light.position.x && packed.data[s * 4][1] == light.position.y)
                    slot = s;
            const bx::Vec3 got = kast::voxelGiLightIrradiance(packed, slot, points[p], normals[p]);
            const bx::Vec3 want = kast::bakedLightIrradiance({.sample = light}, points[p], normals[p]);
            const bool same = slot > 0 && near(got, want, 1e-4f * std::max(1.0f, bx::length(want)));
            if (!same)
                where += std::format(" light {} at point {}: {} vs {};", i, p, got.x, want.x);
            parity = parity && same;
        }
        // And against the renderer's own functions directly, for the point light.
        const bx::Vec3 to = bx::sub(lights[0].position, points[p]);
        const float d = bx::length(to);
        const float direct = kast::pointLightFalloff(d, lights[0].range) * std::max(bx::dot(normals[p], bx::mul(to, 1.0f / d)), 0.0f);
        size_t pointSlot = 0;
        for (size_t s = 1; s < packed.count; ++s)
            if (packed.data[s * 4][3] == float(kast::VoxelGiLightType::Point))
                pointSlot = s;
        parity = parity && near(kast::voxelGiLightIrradiance(packed, pointSlot, points[p], normals[p]),
                                bx::mul(lights[0].color, direct), 1e-5f);
    }
    check(parity, "inject's falloffs (point, spot, area, directional) match the renderer's:" + where);
    bool slots = false;
    for (size_t s = 1; s < packed.count; ++s)
        if (packed.data[s * 4][3] == float(kast::VoxelGiLightType::Spot))
            slots = packed.data[s * 4 + 3][2] == 4.0f && near(packed.data[s * 4 + 3][0], 0.8f, 1e-6f) &&
                    near(packed.data[s * 4 + 2][3], 0.6f, 1e-6f);
    check(slots, "a spot light packs its cone and its shadow tile");

    std::vector<kast::LocalLightSample> many(40, kast::LocalLightSample{.position = {0, 2, -6}, .range = 5, .color = {1, 1, 1}});
    check(kast::packVoxelGiLights(nullptr, {0, 0, 0}, false, many, {}, box).count == kast::kMaxVoxelGiLights &&
              kast::packVoxelGiLights(&sunDir, {0, 0, 0}, true, {}, {}, box).count == 0,
          "at most kMaxVoxelGiLights; a black or missing sun takes no slot");
}

void checkFiles()
{
    const std::array<int, 3> dims{8, 4, 8};
    std::vector<kast::Voxel> voxels{
        {.index = 3, .albedo = {0.8f, 0.1f, 0.05f}, .opacity = 1.0f, .normal = {1, 0, 0}},
        {.index = 40, .albedo = {0.2f, 0.7f, 0.1f}, .opacity = 0.5f, .normal = bx::normalize(bx::Vec3{0, 1, 1}),
         .emission = {12.5f, 0.25f, 0.0f}},
        {.index = 255, .albedo = {1, 1, 1}, .opacity = 1.0f, .normal = {0, -1, 0}, .emission = {0.001f, 0.002f, 0.003f}}};
    const std::string bytes = kast::writeVoxels(dims, voxels);
    std::vector<kast::Voxel> read;
    std::string error;
    bool same = kast::readVoxels(bytes, dims, read, error) && read.size() == voxels.size();
    for (size_t i = 0; same && i < voxels.size(); ++i) {
        const kast::Voxel& a = voxels[i];
        const kast::Voxel& b = read[i];
        const float e = std::max({a.emission.x, a.emission.y, a.emission.z});
        same = a.index == b.index && near(a.albedo, b.albedo, 0.5f / 255.0f + 1e-6f) && near(a.opacity, b.opacity, 1.0f / 255.0f) &&
               near(a.normal, b.normal, 1.0f / 255.0f + 1e-6f) && near(a.emission, b.emission, e / 128.0f + 1e-9f);
    }
    check(same && bytes.size() == 24 + 16 * voxels.size(), ".kvox round trip (8-bit albedo and normal, RGBE emission) " + error);

    std::string truncated = bytes.substr(0, bytes.size() - 3);
    std::string version = bytes;
    version[4] = 7;
    std::string order = bytes;
    std::memcpy(order.data() + 24 + 16, order.data() + 24, 4); // the second voxel's index = the first's
    check(!kast::readVoxels("nope", dims, read, error) && contains(error, "not a .kvox") &&
              !kast::readVoxels(truncated, dims, read, error) && contains(error, "truncated") &&
              !kast::readVoxels(version, dims, read, error) && contains(error, "version 7") &&
              !kast::readVoxels(order, dims, read, error) && contains(error, "out of order") &&
              !kast::readVoxels(bytes, {8, 4, 4}, read, error) && contains(error, "grid"),
          ".kvox rejects garbage, truncation, another version, unordered voxels and another grid: " + error);

    kast::VoxelGiSidecar sidecar{.hash = "0123456789abcdef", .resolution = 64, .voxels = 3, .file = "12.kvox",
                                 .fileHash = kast::lightmapFileHash(bytes)};
    sidecar.grid = {.origin = {-1.0f, 0.5f, -2.0f}, .voxelSize = 0.25f, .dims = dims};
    kast::VoxelGiSidecar parsed;
    check(kast::parseVoxelGiSidecar(kast::voxelGiSidecarJson(sidecar), parsed, error) && parsed.hash == sidecar.hash &&
              parsed.resolution == 64 && parsed.grid == sidecar.grid && parsed.voxels == 3 && parsed.file == "12.kvox" &&
              parsed.fileHash == sidecar.fileHash,
          "sidecar round trip " + error);
    const nlohmann::ordered_json good = nlohmann::ordered_json::parse(kast::voxelGiSidecarJson(sidecar));
    for (const auto& [field, value, expect] :
         {std::tuple("version", "2", "version 2"), std::tuple("resolution", "100", "resolution"),
          std::tuple("dims", "[8, 4]", "dims"), std::tuple("dims", "[8, 4, 65]", "dims"),
          std::tuple("voxelSize", "0", "voxelSize"), std::tuple("origin", "[0, \"a\", 0]", "origin"),
          std::tuple("voxels", "9999", "voxels"), std::tuple("file", "\"../x.kvox\"", "file"),
          std::tuple("fileHash", "\"xyz\"", "fileHash"), std::tuple("hash", "3", "hash")}) {
        nlohmann::ordered_json bad = good;
        bad[field] = nlohmann::ordered_json::parse(value);
        check(!kast::parseVoxelGiSidecar(bad.dump(), parsed, error) && contains(error, expect),
              std::format("the sidecar rejects {} {} ({})", field, value, error));
    }
    check(!kast::parseVoxelGiSidecar("{", parsed, error) && contains(error, "not a VoxelGI sidecar"), "not json: " + error);

    std::vector<kast::Voxel> loaded;
    const std::string json = kast::voxelGiSidecarJson(sidecar);
    check(kast::checkVoxelGiBake(json, bytes, 64, parsed, loaded, error) && loaded.size() == 3, "a matching bake loads " + error);
    check(!kast::checkVoxelGiBake(json, bytes, 128, parsed, loaded, error) && contains(error, "baked at resolution 64"),
          "another resolution is a mismatch: " + error);
    check(!kast::checkVoxelGiBake(json, "not a KVOX", 64, parsed, loaded, error) && contains(error, "doesn't match"),
          "a replaced .kvox doesn't match its hash: " + error);
    check(kast::voxelGiSidecarPath("rooms/voxelgi_room.scene", 13) == "rooms/voxelgi_room_voxelgi/13.json" &&
              kast::voxelGiSidecarPath("a.scene", 2) == "a_voxelgi/2.json",
          "the bake goes beside the scene: <scene-dir>/<scene-name>_voxelgi/<id>.json");
}

void checkVoxelize()
{
    kast::VoxelGiBakeInput input;
    input.grid = kast::voxelGridFor({0, 0.5f, 0}, {2, 1, 2}, 16); // 0.125 m voxels, y from 0 to 1
    input.scene.surfaces.push_back({.albedo = {0.8f, 0.2f, 0.1f}, .emission = {0, 0, 0}});
    input.scene.surfaces.push_back({.albedo = {0.1f, 0.1f, 0.1f}, .emission = {2, 1, 0.5f}});
    kast::BakeEntity floor;
    bx::mtxIdentity(floor.world.data());
    kast::BakeMesh quad;
    quad.positions = {{-1, 0.3f, -1}, {-1, 0.3f, 1}, {1, 0.3f, 1}, {1, 0.3f, -1}};
    quad.normals.assign(4, bx::Vec3{0, 1, 0});
    quad.indices = {0, 1, 2, 0, 2, 3};
    floor.meshes.push_back(quad);
    kast::BakeEntity lamp = floor;
    lamp.meshes[0].surface = 1;
    bx::mtxTranslate(lamp.world.data(), 0.0f, 0.5f, 0.0f); // at y 0.8
    lamp.meshes[0].positions = {{-0.2f, 0.3f, -0.2f}, {-0.2f, 0.3f, 0.2f}, {0.2f, 0.3f, 0.2f}, {0.2f, 0.3f, -0.2f}};
    input.scene.entities = {floor, lamp};
    kast::LightmapBakeProgress progress;
    const std::vector<kast::Voxel> voxels = kast::voxelizeVoxelGi(input, progress);
    size_t floorLayer = 0, lampLayer = 0;
    bool fields = true, sorted = std::ranges::is_sorted(voxels, {}, &kast::Voxel::index);
    for (const kast::Voxel& v : voxels) {
        const auto c = input.grid.coord(v.index);
        fields = fields && near(v.normal, {0, 1, 0}, 1e-5f) && v.opacity == 1.0f;
        if (c[1] == 2) { // y 0.3 / 0.125
            ++floorLayer;
            fields = fields && near(v.albedo, {0.8f, 0.2f, 0.1f}, 1e-5f) && v.emission.x == 0.0f;
        } else if (c[1] == 6) { // y 0.8
            ++lampLayer;
            fields = fields && near(v.emission, {2, 1, 0.5f}, 1e-5f);
        } else {
            fields = false;
        }
    }
    check(floorLayer == 16 * 16 && lampLayer == 4 * 4 && voxels.size() == floorLayer + lampLayer && fields && sorted,
          std::format("a quad fills its layer of voxels with its surface ({} floor, {} lamp voxels)", floorLayer, lampLayer));
    check(kast::voxelizeVoxelGi(input, progress).size() == voxels.size(), "the voxelizer is deterministic");
    input.scene.entities.clear();
    check(kast::voxelizeVoxelGi(input, progress).empty(), "no Static geometry: an empty volume");
    progress.cancel.store(true);
    kast::VoxelGiBakeOutput out;
    std::string error;
    input.scene.entities = {floor};
    check(!kast::bakeVoxelGi(input, progress, out, error) && error == "cancelled", "Cancel stops the bake");
}

void checkScene()
{
    kast::Scene scene;
    const entt::entity wall = scene.create("Wall");
    scene.registry().emplace<kast::MeshRenderer>(wall, kast::MeshRenderer{.giMode = kast::GiMode::Static});
    const entt::entity ball = scene.create("Ball");
    scene.registry().emplace<kast::MeshRenderer>(ball, kast::MeshRenderer{.giMode = kast::GiMode::Dynamic});
    const entt::entity volume = scene.create("Volume");
    scene.registry().emplace<kast::VoxelGI>(volume);
    const std::string before = kast::voxelGiSceneHash(scene, volume);
    scene.registry().get<kast::Transform>(ball).position.x += 1.0f;
    const std::string dynamicMoved = kast::voxelGiSceneHash(scene, volume);
    scene.registry().get<kast::Transform>(wall).position.x += 1.0f;
    const std::string staticMoved = kast::voxelGiSceneHash(scene, volume);
    scene.registry().get<kast::VoxelGI>(volume).resolution = 64;
    check(before == dynamicMoved && dynamicMoved != staticMoved && staticMoved != kast::voxelGiSceneHash(scene, volume),
          "the bake's hash: a Static mesh moving or the resolution changing changes it, a Dynamic mesh doesn't");
    const std::string withResolution = kast::voxelGiSceneHash(scene, volume);
    scene.registry().get<kast::VoxelGI>(volume).intensity = 0.5f;
    scene.registry().get<kast::VoxelGI>(volume).enabled = false;
    check(withResolution == kast::voxelGiSceneHash(scene, volume), "intensity and enabled need no bake");

    kast::VoxelGI& v = scene.registry().get<kast::VoxelGI>(volume);
    v.quality = kast::VoxelGiQuality::High;
    v.priority = 2;
    v.bakedPath = "room_voxelgi/3.json";
    kast::Scene loaded;
    std::string error;
    const nlohmann::ordered_json json = kast::sceneToJson(scene);
    const kast::VoxelGI* back = nullptr;
    if (kast::sceneFromJson(json, loaded, error))
        for (auto [entity, component] : loaded.registry().view<kast::VoxelGI>().each())
            back = &component;
    check(back && back->resolution == 64 && back->quality == kast::VoxelGiQuality::High && back->priority == 2 &&
              !back->enabled && back->intensity == 0.5f && back->bakedPath == v.bakedPath &&
              json["entities"][2]["components"]["VoxelGI"]["quality"] == "high",
          "VoxelGI round trip, quality by name: " + error);
    for (const auto& [field, value] : {std::pair("resolution", "100"), std::pair("intensity", "-1"),
                                       std::pair("boxSize", "[1, 0, 1]"), std::pair("quality", "\"ultra\"")}) {
        nlohmann::ordered_json bad = json;
        bad["entities"][2]["components"]["VoxelGI"][field] = nlohmann::ordered_json::parse(value);
        kast::Scene rejected;
        check(!kast::sceneFromJson(bad, rejected, error) && contains(error, field),
              std::format("VoxelGI rejects {} {} ({})", field, value, error));
    }

    check(kast::selectDiffuseGi(true, true, true) == kast::DiffuseGi::Lightmap &&
              kast::selectDiffuseGi(false, true, true) == kast::DiffuseGi::VoxelGi &&
              kast::selectDiffuseGi(false, true, false) == kast::DiffuseGi::ProbeGrid &&
              kast::selectDiffuseGi(false, false, true) == kast::DiffuseGi::VoxelGi &&
              kast::selectDiffuseGi(false, false) == kast::DiffuseGi::Environment,
          "the diffuse precedence: lightmap > VoxelGI > probe grid > environment");
    kast::ShaderSource source;
    check(kast::parseShader("param texture2D a;\nvoid fragment() {}\n", source, error) && kast::shaderTakesVoxelGi(source) &&
              kast::parseShader("param texture2D a;\nparam texture2D b;\nvoid fragment() {}\n", source, error) &&
              !kast::shaderTakesVoxelGi(source) && kast::shaderTakesLightmap(source),
          ".kshaders with a second texture2D param take VoxelGI's stage (11)");
}

} // namespace

int voxelGiSelfCheck()
{
    g_failures = 0;
    checkGrid();
    checkSelection();
    checkCones();
    checkLights();
    checkFiles();
    checkVoxelize();
    checkScene();
    kast::log::info(std::format("voxelgi_self_check: {} failure(s)", g_failures));
    return g_failures;
}
