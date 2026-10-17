#include "zone_view.h"

#include <math3d.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace zone {
namespace {

namespace km = kast::map;
constexpr uint32_t kClearColor = 0x000000ff; // the palette's background
constexpr ImU32 kTileLine = IM_COL32(255, 170, 0, 140);
constexpr ImU32 kSpawnColor = IM_COL32(255, 204, 0, 255);
constexpr float kBorderStep = 4.0f; // metres between the points of a tile border line

} // namespace

void ZoneView::init() {
    renderer_.init();
    post_.init();
    camera.nearPlane = 0.5f; // depth precision over a 2 km+ view
    camera.farPlane = 6000.0f;
    camera.pitch = -35.0f;
    const entt::entity sun = scene_.create("Sun");
    scene_.registry().get<kast::Transform>(sun).rotation = kast::quatFromEulerDeg({-50.0f, 30.0f, 0.0f});
    // ponytail: no sun shadows; they cost two passes over every tile and the editor view does not need them.
    scene_.registry().emplace<kast::DirectionalLight>(sun).castShadows = false;
    scene_.lighting.ambientIntensity = 0.35f;
}

void ZoneView::shutdown() {
    post_.shutdown();
    renderer_.shutdown();
}

void ZoneView::sync(const ZoneDoc& doc) {
    if (doc.state.manifest != manifest_) {
        manifest_ = doc.state.manifest;
        std::string err;
        auto info = doc.info(err);
        info_ = info ? std::make_shared<const km::MapInfo>(std::move(*info)) : nullptr;
        for (auto& [key, tile] : tiles_) tile.built = false; // every tile's data changes with the manifest
    }
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (info_ && info_->has_tile(it->first)) {
            ++it;
            continue;
        }
        scene_.registry().destroy(it->second.entity);
        it = tiles_.erase(it);
    }
    if (!info_) return;
    for (const km::TileRect& r : info_->tiles)
        for (int32_t z = r.z0; z <= r.z1; ++z)
            for (int32_t x = r.x0; x <= r.x1; ++x) {
                const TileKey key{x, z};
                const auto found = doc.state.heights.find(key);
                const HeightsPtr heights = found == doc.state.heights.end() ? nullptr : found->second;
                Tile& tile = tiles_[key];
                if (tile.built && tile.heights == heights) continue;
                if (tile.entity == entt::null) {
                    tile.entity = scene_.registry().create();
                    scene_.registry().emplace<kast::TerrainTile>(tile.entity, int(info_->id), x, z);
                }
                auto data = std::make_shared<kast::TerrainTileData>();
                data->map = info_;
                data->tile = key;
                data->heights = heights ? *heights : km::flat_heights();
                scene_.registry().get<kast::TerrainTile>(tile.entity).data = std::move(data);
                tile.heights = heights;
                tile.built = true;
            }
}

void ZoneView::render(const ZoneDoc* doc, const std::filesystem::path& projectRoot, uint16_t width, uint16_t height) {
    if (width == 0 || height == 0) return;
    if (doc) sync(*doc);
    else if (!tiles_.empty()) {
        for (auto& [key, tile] : tiles_) scene_.registry().destroy(tile.entity);
        tiles_.clear();
        manifest_ = nullptr;
    }
    width_ = width;
    height_ = height;
    float view[16], proj[16];
    camera.viewMatrix(view);
    camera.projMatrix(proj, float(width) / float(height));
    bx::mtxMul(viewProj_, view, proj);
    post_.begin(width, height, kast::sceneClearColor(scene_.postProcessing, kClearColor));
    renderer_.render(scene_, projectRoot, post_.sceneView(), view, proj, shadowViews_);
    post_.apply(scene_.postProcessing, true, BGFX_INVALID_HANDLE);
}

void ZoneView::updateCamera(float viewportHeight) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && drag_ == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt))
            drag_ = Drag::Orbit;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            drag_ = Drag::Pan;
        else if (io.MouseWheel != 0.0f)
            camera.dolly(io.MouseWheel);
    }
    const ImVec2 d = io.MouseDelta;
    if (drag_ == Drag::Orbit) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) camera.orbit(d.x, d.y);
        else drag_ = Drag::None;
    } else if (drag_ == Drag::Pan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) camera.pan(d.x, d.y, viewportHeight);
        else drag_ = Drag::None;
    }
}

void ZoneView::frameMap(const ZoneDoc& doc) {
    std::string err;
    const auto info = doc.info(err);
    if (!info) return;
    const auto range = km::height_range(*info);
    kast::Aabb box{{std::numeric_limits<float>::max(), range[0], std::numeric_limits<float>::max()},
                   {std::numeric_limits<float>::lowest(), range[1], std::numeric_limits<float>::lowest()}};
    for (const km::TileRect& r : info->tiles) {
        box.min = bx::min(box.min, {r.x0 * km::kTileSize, range[0], r.z0 * km::kTileSize});
        box.max = bx::max(box.max, {(r.x1 + 1) * km::kTileSize, range[1], (r.z1 + 1) * km::kTileSize});
    }
    camera.focus(box);
}

std::optional<Vec3> ZoneView::pick(const ZoneDoc& doc, float x, float y) const {
    std::string err;
    const auto info = doc.info(err);
    if (!info) return std::nullopt;
    const kast::Ray ray = kast::screenRay(viewProj_, x, y, width_, height_, bgfx::getCaps()->homogeneousDepth);
    return raycast(*info, doc.state, {ray.origin.x, ray.origin.y, ray.origin.z},
                   {ray.direction.x, ray.direction.y, ray.direction.z}, camera.farPlane);
}

std::optional<std::array<float, 2>> ZoneView::project(Vec3 p) const {
    float clip[4];
    const float v[4] = {p[0], p[1], p[2], 1.0f};
    bx::vec4MulMtx(clip, v, viewProj_);
    if (clip[3] <= 1e-4f) return std::nullopt;
    // ImGui works in window points; the backbuffer may be scaled (high DPI).
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    return std::array<float, 2>{(clip[0] / clip[3] * 0.5f + 0.5f) * width_ / scale.x,
                                (0.5f - clip[1] / clip[3] * 0.5f) * height_ / scale.y};
}

Vec3 ZoneView::groundPoint(const ZoneDoc& doc, float x, float z) const {
    if (!info_) return {x, 0.0f, z};
    // A point on the map's far edge belongs to the tile beyond it: sample the nearest point on the map.
    const auto c = info_->clamp(x, z);
    return Vec3{x, ground_y(*info_, doc.state, c[0], c[1]).value_or(0.0f) + 0.2f, z};
}

void ZoneView::drawGroundLine(const ZoneDoc& doc, std::array<float, 2> a, std::array<float, 2> b, ImU32 color,
                              float thickness) const {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const int steps = std::max(1, int(std::hypot(b[0] - a[0], b[1] - a[1]) / kBorderStep));
    std::optional<std::array<float, 2>> last;
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const auto p = project(groundPoint(doc, a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t));
        if (p && last) draw->AddLine({(*last)[0], (*last)[1]}, {(*p)[0], (*p)[1]}, color, thickness);
        last = p;
    }
}

void ZoneView::drawGroundCircle(const ZoneDoc& doc, float x, float z, float radius, ImU32 color) const {
    constexpr int kSegments = 64;
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = 2.0f * bx::kPi * float(i) / kSegments, a1 = 2.0f * bx::kPi * float(i + 1) / kSegments;
        drawGroundLine(doc, {x + radius * std::cos(a0), z + radius * std::sin(a0)},
                       {x + radius * std::cos(a1), z + radius * std::sin(a1)}, color, 2.0f);
    }
}

void ZoneView::drawOverlays(const ZoneDoc& doc) const {
    if (!info_) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const auto ground = [&](float x, float z) { return groundPoint(doc, x, z); };
    for (const auto& [key, tile] : tiles_) {
        const float x0 = key.x * km::kTileSize, z0 = key.z * km::kTileSize, x1 = x0 + km::kTileSize, z1 = z0 + km::kTileSize;
        drawGroundLine(doc, {x0, z0}, {x1, z0}, kTileLine, 1.5f);
        drawGroundLine(doc, {x0, z0}, {x0, z1}, kTileLine, 1.5f);
        if (!info_->has_tile({key.x, key.z + 1})) drawGroundLine(doc, {x0, z1}, {x1, z1}, kTileLine, 1.5f);
        if (!info_->has_tile({key.x + 1, key.z})) drawGroundLine(doc, {x1, z0}, {x1, z1}, kTileLine, 1.5f);
    }
    const Vec3 spawn = ground(info_->spawn[0], info_->spawn[2]);
    if (const auto p = project(spawn)) {
        draw->AddCircleFilled({(*p)[0], (*p)[1]}, 6.0f, kSpawnColor);
        const float h = info_->spawnHeading;
        if (const auto q = project(ground(spawn[0] + 6.0f * std::sin(h), spawn[2] + 6.0f * std::cos(h))))
            draw->AddLine({(*p)[0], (*p)[1]}, {(*q)[0], (*q)[1]}, kSpawnColor, 3.0f);
        draw->AddText({(*p)[0] + 8.0f, (*p)[1] - 8.0f}, kSpawnColor, "spawn");
    }
}

} // namespace zone
