#include "map_tiles.h"

#include "assets.h" // utf8, fromUtf8
#include "ui_common.h"

#include <mapedit/terrain_edit.h>
#include <mapedit/zone_edit.h>

#include <components.h>
#include <files.h> // readFile
#include <log.h>
#include <project.h> // toProjectPath
#include <zone_scenes.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace {

namespace km = kast::map;

constexpr double kRefreshSeconds = 0.5; // the scene files and counts are read again this often (file changes)
constexpr int kThumbnailSize = 64;
constexpr size_t kMaxThumbnails = 1024; // visible tiles at most (a texture each), well under bgfx's 4096 handles
constexpr float kMinThumbnailCell = 8.0f; // pixels: smaller cells show their state color only
constexpr int kThumbnailsPerFrame = 64; // built a frame at most: a zoom that shows hundreds stays smooth
constexpr float kMaxCell = 96.0f; // pixels: the widest a cell fits or zooms to
constexpr ImU32 kButton = kAccent; // an open tile scene
constexpr ImU32 kBlack = IM_COL32(0, 0, 0, 255);
// Not ImGui's text color, so darkenTextOnAccent leaves cell labels on an open cell's accent tint white.
constexpr ImU32 kWhite = IM_COL32(250, 250, 250, 255);
constexpr ImU32 kZoneBorder = kWhite; // between tiles of different zones (kWhite: see above)
constexpr float kZoneBorderWidth = 4.0f;
constexpr ImU32 kZoneSelected = withAlpha(kHighlight, 70); // the selected zone's tint

ImU32 borderColor(kast::TileCell cell)
{
    switch (cell) {
    case kast::TileCell::Declared: return kField;
    case kast::TileCell::HasScene: return kPressed;
    case kast::TileCell::Open: return kButton;
    case kast::TileCell::Unsaved: return kHighlight;
    case kast::TileCell::Orphan: return kError;
    case kast::TileCell::Undeclared: break;
    }
    return kCard;
}

float borderWidth(kast::TileCell cell)
{
    return cell == kast::TileCell::Open || cell == kast::TileCell::Unsaved ? 3.0f
           : cell == kast::TileCell::Declared || cell == kast::TileCell::Undeclared ? 1.0f
                                                                                    : 2.0f;
}

void legendItem(ImU32 color, const char* label)
{
    const float size = ImGui::GetTextLineHeight() * 0.8f;
    const float width = size + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
    if (ImGui::GetContentRegionAvail().x < width)
        ImGui::NewLine();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y + (ImGui::GetTextLineHeight() - size) * 0.5f;
    ImGui::GetWindowDrawList()->AddRect({at.x, y}, {at.x + size, y + size}, color, 0.0f, 0, 2.0f);
    ImGui::Dummy({size, ImGui::GetTextLineHeight()});
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
}

} // namespace

std::filesystem::path MapTiles::tileFile(const std::filesystem::path& root, const std::string& folder, km::TileKey t) const
{
    return (root / fromUtf8(kast::tileScenePath(folder, t))).lexically_normal();
}

std::optional<MapTiles::Mode> MapTiles::modeNamed(std::string_view name)
{
    if (name == "scenes")
        return Mode::Scenes;
    if (name == "tiles")
        return Mode::Tiles;
    if (name == "zones")
        return Mode::Zones;
    return std::nullopt;
}

void MapTiles::update(const Document& doc, const MapSession& session)
{
    const mapedit::MapDoc* z = session.doc();
    std::string err;
    const std::optional<km::MapInfo> info = z ? z->info(err) : std::nullopt;
    // In Play the open scenes are the streamed ones; in prefab edit mode doc.scene is the prefab, so the last state stays.
    if (doc.project && session.terrainEntity() != entt::null && info && !info->tiles.empty() && !doc.prefabEdit &&
        (z->folder != folder_ || doc.sceneGeneration != generation_ || ImGui::GetTime() >= nextRefresh_))
        refresh(doc, *z, *info);
}

std::optional<km::TileKey> MapTiles::draw(Document& doc, const MapSession& session, FileActions& files,
                                          const km::MapInfo& info, bool locked, const std::string& zone)
{
    ImGui::TextUnformatted("Click:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Tile scenes", mode_ == Mode::Scenes))
        mode_ = Mode::Scenes;
    ImGui::SameLine();
    if (ImGui::RadioButton("Add / remove tiles", mode_ == Mode::Tiles))
        mode_ = Mode::Tiles;
    ImGui::SameLine();
    if (ImGui::RadioButton("Paint zone", mode_ == Mode::Zones))
        mode_ = Mode::Zones;
    const std::optional<km::TileKey> clicked = drawGrid(doc, session, files, info, locked, zone);
    drawWarnings();
    return clicked;
}

void MapTiles::refresh(const Document& doc, const mapedit::MapDoc& z, const km::MapInfo& info)
{
    const std::filesystem::path& root = doc.project->root;
    const kast::TileSceneFiles listing = kast::listTileScenes(root / "maps" / fromUtf8(z.folder));
    sceneFiles_ = {listing.tiles.begin(), listing.tiles.end()};
    if (z.folder != folder_)
        kast::log::info(std::format("Map: maps/{} ({} tiles, {} tile scenes)", z.folder,
                                    mapedit::declared_tiles(info).size(), sceneFiles_.size()));
    logView_ = logView_ || z.folder != folder_; // a new map: its first view too
    folder_ = z.folder;
    generation_ = doc.sceneGeneration;
    nextRefresh_ = ImGui::GetTime() + kRefreshSeconds;

    // The tile scenes: their files, and open ones not saved yet (a created cell).
    std::set<km::TileKey, mapedit::TileLess> tiles = sceneFiles_;
    for (const kast::LoadedScene& loaded : doc.scene.loadedScenes()) {
        const std::string path = scenePath(doc, loaded.slot);
        const std::optional<std::string> relative = path.empty() ? std::nullopt : kast::toProjectPath(root, fromUtf8(path));
        if (const std::optional<kast::TileSceneRef> ref = relative ? kast::parseTileScenePath(*relative) : std::nullopt;
            ref && ref->folder == z.folder)
            tiles.insert(ref->tile);
    }
    std::map<kast::SceneSlot, size_t> perScene;
    for (auto [entity, member] : doc.scene.registry().view<kast::SceneMember>().each())
        ++perScene[member.slot];

    const mapedit::TileSet declared = mapedit::declared_tiles(info);
    entities_.clear();
    warnings_.clear();
    for (const km::TileKey t : tiles) {
        const std::string rel = kast::tileScenePath(z.folder, t);
        const std::optional<SceneRoots> scene = sceneRoots(doc, rel, perScene, warnings_);
        if (!scene)
            continue;
        entities_[t] = scene->entities;
        if (!declared.contains(t))
            warnings_.push_back(std::format("{} is an orphan: tile {} is not a map tile (declare it in the Map tab, "
                                            "or move its entities into another tile scene)",
                                            rel, mapedit::tile_name(t)));
        for (const kast::TileSceneRoot& r : scene->roots)
            if (const std::optional<km::TileKey> at = kast::outsideTile(t, r.xz[0], r.xz[1]))
                warnings_.push_back(std::format("{}: root entity '{}' at ({:.0f}, {:.0f}) is in tile {}, outside tile {}",
                                                rel, r.name, r.xz[0], r.xz[1], mapedit::tile_name(*at), mapedit::tile_name(t)));
    }
    for (const std::string& name : listing.badNames)
        warnings_.push_back(std::format("maps/{}/scenes/{}: not a tile scene name (<tx>_<tz>.scene), ignored", z.folder, name));

    // The zone scenes: their files and open ones, against the zones as edited (a renamed zone's file keeps its old id
    // until save).
    const kast::ZoneSceneFiles zoneListing = kast::listZoneScenes(root / "maps" / fromUtf8(z.folder));
    zoneSceneFiles_ = {zoneListing.zones.begin(), zoneListing.zones.end()};
    std::set<std::string> zoneScenes = zoneSceneFiles_;
    for (const kast::LoadedScene& loaded : doc.scene.loadedScenes()) {
        const std::string path = scenePath(doc, loaded.slot);
        const std::optional<std::string> relative = path.empty() ? std::nullopt : kast::toProjectPath(root, fromUtf8(path));
        if (const std::optional<kast::ZoneSceneRef> ref = relative ? kast::parseZoneScenePath(*relative) : std::nullopt;
            ref && ref->folder == z.folder)
            zoneScenes.insert(ref->zone);
    }
    const km::Zones zones = mapedit::map_zones(info, z.state, z.folder);
    std::map<std::string, int16_t> zoneOfScene;
    for (size_t i = 0; i < zones.list.size(); ++i)
        zoneOfScene.emplace(mapedit::zone_scene_id(z.state, zones.list[i].id), int16_t(i));
    zoneEntities_.clear();
    zoneWarnings_.clear();
    for (const std::string& id : zoneScenes) {
        const std::string rel = kast::zoneScenePath(z.folder, id);
        const std::optional<SceneRoots> scene = sceneRoots(doc, rel, perScene, zoneWarnings_);
        if (!scene)
            continue;
        zoneEntities_[id] = scene->entities;
        const auto zone = zoneOfScene.find(id);
        if (zone == zoneOfScene.end()) {
            zoneWarnings_.push_back(std::format("{} is an orphan: the map has no zone \"{}\" (rename the file to a zone's "
                                                "id, or move its entities into another zone scene)",
                                                rel, id));
            continue;
        }
        for (const kast::TileSceneRoot& r : scene->roots)
            if (const int16_t at = km::zone_at(zones, r.xz[0], r.xz[1]); at != zone->second)
                zoneWarnings_.push_back(std::format("{}: root entity '{}' at ({:.0f}, {:.0f}) is {}, outside zone {}", rel,
                                                    r.name, r.xz[0], r.xz[1],
                                                    at < 0 ? std::string("on no zone's tile")
                                                           : std::format("in zone {}", zones.list[size_t(at)].id),
                                                    zones.list[size_t(zone->second)].id));
    }
    for (const std::string& name : zoneListing.badNames)
        zoneWarnings_.push_back(
            std::format("maps/{}/zones/{}: not a zone scene name (<zoneId>.scene), ignored", z.folder, name));

    std::set<std::string> shown(warnings_.begin(), warnings_.end());
    shown.insert(zoneWarnings_.begin(), zoneWarnings_.end());
    for (const std::vector<std::string>* list : {&warnings_, &zoneWarnings_})
        for (const std::string& warning : *list)
            if (!logged_.contains(warning))
                kast::log::warn("Map: " + warning);
    logged_ = std::move(shown);
}

std::optional<MapTiles::SceneRoots> MapTiles::sceneRoots(const Document& doc, const std::string& rel,
                                                         const std::map<kast::SceneSlot, size_t>& perScene,
                                                         std::vector<std::string>& warnings)
{
    const std::filesystem::path file = (doc.project->root / fromUtf8(rel)).lexically_normal();
    SceneRoots out;
    if (const std::optional<kast::SceneSlot> slot = findOpenScene(doc, utf8(file))) {
        const auto count = perScene.find(*slot);
        out.entities = count == perScene.end() ? 0 : count->second;
        for (const entt::entity entity : doc.scene.children(entt::null))
            if (doc.scene.sceneOf(entity) == *slot) {
                const bx::Vec3& p = doc.scene.registry().get<kast::Transform>(entity).position; // a root's is world
                out.roots.push_back({doc.scene.registry().get<kast::Name>(entity).value, {p.x, p.z}});
            }
        return out;
    }
    std::error_code ec;
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(file, ec);
    FileSummary& summary = files_[rel];
    if ((!summary.summary && summary.error.empty()) || summary.mtime != mtime) {
        std::string text;
        summary = {mtime, std::nullopt, ""};
        if (!kast::readFile(file, text))
            summary.error = "cannot read the file";
        else
            summary.summary = kast::summarizeTileScene(text, summary.error);
    }
    if (!summary.summary) {
        warnings.push_back(std::format("{}: {}", rel, summary.error));
        return std::nullopt;
    }
    out.entities = summary.summary->entities;
    out.roots = summary.summary->roots;
    return out;
}

void MapTiles::syncThumbnails(const mapedit::MapDoc& z, const km::MapInfo& info)
{
    const std::string colors = z.state.manifest.value("terrainColors", mapedit::Json()).dump();
    if (colors != thumbnailColors_)
        shutdown(); // new colors: every thumbnail again (another map's tiles have their own heights buffers)
    thumbnailColors_ = colors;
    std::string err;
    const std::optional<mapedit::Gradient> gradient =
        z.state.manifest.contains("terrainColors") ? mapedit::parse_gradient(z.state.manifest, err) : std::nullopt;
    // Only the visible cells', and none while they are too small or too many to show one (bgfx has 4096 texture
    // handles in all): plain cells.
    const km::TileRect shown = grid_.visible();
    mapedit::TileSet visible;
    if (grid_.view().cell >= kMinThumbnailCell)
        for (int32_t tz = shown.z0; tz <= shown.z1; ++tz)
            for (int32_t tx = shown.x0; tx <= shown.x1; ++tx)
                if (info.has_tile({tx, tz}))
                    visible.insert({tx, tz});
    if (visible.size() > kMaxThumbnails)
        visible.clear();
    std::erase_if(thumbnails_, [&](auto& entry) {
        if (visible.contains(entry.first))
            return false;
        bgfx::destroy(entry.second.texture);
        return true;
    });
    thumbnailsPending_ = 0;
    int built = 0;
    for (const km::TileKey t : visible) {
        const auto it = z.state.heights.find(t);
        const mapedit::HeightsPtr heights = it == z.state.heights.end() ? nullptr : it->second;
        const auto have = thumbnails_.find(t);
        if (have != thumbnails_.end() && have->second.heights == heights)
            continue; // a Map tab edit makes a new buffer (copy on write)
        if (++built > kThumbnailsPerFrame) {
            ++thumbnailsPending_;
            continue;
        }
        Thumbnail& thumbnail = thumbnails_[t];
        if (bgfx::isValid(thumbnail.texture))
            bgfx::destroy(thumbnail.texture);
        const mapedit::TerrainImage image = mapedit::tile_thumbnail(z.state, t, gradient, kThumbnailSize);
        const bgfx::Memory* rgba = bgfx::alloc(uint32_t(image.width * image.height * 4));
        for (size_t i = 0; i < size_t(image.width * image.height); ++i) {
            std::copy_n(&image.rgb[i * 3], 3, &rgba->data[i * 4]);
            rgba->data[i * 4 + 3] = 255;
        }
        thumbnail = {heights, bgfx::createTexture2D(uint16_t(image.width), uint16_t(image.height), false, 1,
                                                    bgfx::TextureFormat::RGBA8,
                                                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, rgba)};
    }
}

void MapTiles::shutdown()
{
    for (auto& [tile, thumbnail] : thumbnails_)
        if (bgfx::isValid(thumbnail.texture))
            bgfx::destroy(thumbnail.texture);
    thumbnails_.clear();
}

std::optional<km::TileKey> MapTiles::drawGrid(Document& doc, const MapSession& session, FileActions& files,
                                              const km::MapInfo& info, bool locked, const std::string& zone)
{
    const mapedit::MapDoc& z = *session.doc();
    const bool tiles = mode_ == Mode::Tiles, paint = mode_ == Mode::Zones;
    const km::Zones zones = mapedit::map_zones(info, z.state, z.folder);
    // Scenes: the bounds' cells, at least kMinGrid a side from the bottom-left one (a small map shows the cells around
    // it). Tiles: the bounds and a free cell around them, smaller cells.
    const km::TileRect box = mapedit::tile_bounds(info);
    const int grid = std::max({kMinGrid, box.x1 - box.x0 + 1, box.z1 - box.z0 + 1});
    const km::TileRect bounds = tiles ? km::TileRect{box.x0 - 1, box.z0 - 1, box.x1 + 1, box.z1 + 1}
                                      : km::TileRect{box.x0, box.z0, box.x0 + grid - 1, box.z0 + grid - 1};
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(tiles   ? "maps/%s: click a free cell (+) to add a tile, a tile to remove it; the wheel zooms, a drag "
                                 "pans. +z is up."
                       : paint ? "maps/%s: click or drag over tiles to paint them into the selected zone (Zones section); "
                                 "the wheel zooms, a right drag pans. +z is up."
                               : "maps/%s: click a tile to open, close or create its scene; the wheel zooms, a drag pans. "
                                 "+z is up.",
                       z.folder.c_str());
    ImGui::PopStyleColor();
    grid_.fit(z.folder + (tiles ? "#tiles" : paint ? "#zones" : "#scenes"), bounds, bounds.x1 - bounds.x0 + 1, bounds.z1 - bounds.z0 + 1,
              ImGui::GetContentRegionAvail().x, tiles ? ImGui::GetFrameHeight() * 1.6f : kMaxCell);
    const bool looked = tiles && std::exchange(looked_, false);
    if (looked)
        ImGui::SetScrollHereY(0.0f);
    if (!tiles)
        looked_ = false;
    const TileGrid::Input input = grid_.area("##tiles", paint);
    syncThumbnails(z, info);
    const km::TileRect shown = grid_.visible();
    const float cell = grid_.view().cell;
    if (looked)
        kast::log::info(std::format("Map: tile grid view {}..{} ({:.0f} px cells)", mapedit::tile_name({shown.x0, shown.z0}),
                                    mapedit::tile_name({shown.x1, shown.z1}), cell));
    if (!tiles && logView_ && thumbnailsPending_ == 0) {
        logView_ = false;
        kast::log::info(std::format("Map: view {}..{} ({:.0f} px cells, {} thumbnails)", mapedit::tile_name({shown.x0, shown.z0}),
                                    mapedit::tile_name({shown.x1, shown.z1}), cell, thumbnails_.size()));
    }
    // A map position on screen: inside its tile's cell (the gaps between cells are not ground).
    const auto toScreen = [&](float x, float zz) {
        const km::TileKey t = km::tile_of(x, zz);
        const ImVec2 min = grid_.corner(t);
        return ImVec2(min.x + (x / km::kTileSize - float(t.x)) * cell, min.y + (float(t.z + 1) - zz / km::kTileSize) * cell);
    };

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(grid_.min(), grid_.max(), true);
    const std::filesystem::path& root = doc.project->root;
    for (int32_t tz = shown.z1; tz >= shown.z0; --tz)
        for (int32_t tx = shown.x0; tx <= shown.x1; ++tx) {
            const km::TileKey t{tx, tz};
            const std::optional<kast::SceneSlot> slot = findOpenScene(doc, utf8(tileFile(root, z.folder, t)));
            const kast::TileCell state = kast::tileCell(info.has_tile(t), sceneFiles_.contains(t), slot.has_value(),
                                                        slot && sceneDirty(doc, *slot));
            const ImVec2 min = grid_.corner(t), max{min.x + cell, min.y + cell};
            const auto thumbnail = thumbnails_.find(t);
            if (thumbnail != thumbnails_.end() && bgfx::isValid(thumbnail->second.texture))
                draw->AddImage(static_cast<ImTextureID>(thumbnail->second.texture.idx), min, max);
            else
                draw->AddRectFilled(min, max, state == kast::TileCell::Declared ? kField : kCard);
            if (state == kast::TileCell::Open || state == kast::TileCell::Unsaved) // open in the editor: tinted
                draw->AddRectFilled(min, max, (borderColor(state) & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 120));
            const bool fits = cell >= 24.0f; // the smallest cell labels and counts were laid out for
            const std::string label = mapedit::tile_name(t) + (state == kast::TileCell::Unsaved ? "*" : "");
            const ImVec2 text = ImGui::CalcTextSize(label.c_str());
            if (fits && text.x + 4.0f <= cell) { // long names (two-digit tiles) need a wider cell
                if (state != kast::TileCell::Undeclared)
                    draw->AddRectFilled(min, {min.x + text.x + 4.0f, min.y + text.y}, IM_COL32(0, 0, 0, 160));
                draw->AddText({min.x + 2.0f, min.y}, state == kast::TileCell::Undeclared ? kTextSecondary
                                                     : state == kast::TileCell::Orphan  ? kError
                                                                                         : kWhite,
                              label.c_str());
            }
            if (const auto count = entities_.find(t); fits && count != entities_.end()) {
                const std::string n = std::to_string(count->second);
                const ImVec2 size = ImGui::CalcTextSize(n.c_str());
                const ImVec2 at{max.x - size.x - 2.0f, max.y - size.y};
                draw->AddRectFilled({at.x - 2.0f, at.y}, max, IM_COL32(0, 0, 0, 160));
                draw->AddText(at, kWhite, n.c_str());
            }
            const bool addable = tiles && !info.has_tile(t) && mapedit::within_cap(info, t);
            if (addable) {
                const ImVec2 plus = ImGui::CalcTextSize("+");
                draw->AddText({(min.x + max.x - plus.x) * 0.5f, (min.y + max.y - plus.y) * 0.5f}, kWhite, "+");
            }
            draw->AddRect(min, max, borderColor(state), 0.0f, 0, borderWidth(state));
            if (input.hover == t) {
                const int16_t in = km::zone_at(zones, t);
                if (tiles && !info.has_tile(t) && !addable)
                    ImGui::SetTooltip("A map is at most %d tiles a side", mapedit::kMaxMapTiles);
                else if (paint)
                    ImGui::SetTooltip("Tile %s\nZone: %s%s", mapedit::tile_name(t).c_str(),
                                      in < 0 ? "(none)" : zones.list[size_t(in)].id.c_str(),
                                      zone.empty() ? "\nSelect a zone in the Zones section to paint" : "");
                else if (tiles)
                    ImGui::SetTooltip("%s tile %s", addable ? "Add" : "Remove", mapedit::tile_name(t).c_str());
                else
                    ImGui::SetTooltip("%s\n%s%s", kast::tileScenePath(z.folder, t).c_str(), kast::tileCellName(state),
                                      entities_.contains(t) ? std::format("\n{} entities", entities_.at(t)).c_str() : "");
                if (addable || (state != kast::TileCell::Undeclared && state != kast::TileCell::Orphan))
                    draw->AddRect({min.x - 1.0f, min.y - 1.0f}, {max.x + 1.0f, max.y + 1.0f}, kHighlight, 0.0f, 0, 1.0f);
            }
        }

    // The portal areas (clipped to each visible cell they cross) and the player spawn with its heading.
    std::string err;
    std::optional<km::Areas> areas = z.state.areas.is_null() ? std::nullopt : km::parse_areas(nlohmann::json(z.state.areas), err);
    for (const km::Area& area : areas ? areas->list : std::vector<km::Area>()) {
        if (!area.portal)
            continue;
        const auto [x0, z0, x1, z1] = area.bounds;
        for (int32_t tz = std::max(shown.z0, km::tile_of(x0, z0).z); tz <= std::min(shown.z1, km::tile_of(x1, z1).z); ++tz)
            for (int32_t tx = std::max(shown.x0, km::tile_of(x0, z0).x); tx <= std::min(shown.x1, km::tile_of(x1, z1).x); ++tx) {
                const float cx0 = std::max(x0, float(tx) * km::kTileSize), cx1 = std::min(x1, float(tx + 1) * km::kTileSize);
                const float cz0 = std::max(z0, float(tz) * km::kTileSize), cz1 = std::min(z1, float(tz + 1) * km::kTileSize);
                const ImVec2 min = grid_.corner({tx, tz});
                const ImVec2 a{min.x + (cx0 / km::kTileSize - float(tx)) * cell, min.y + (float(tz + 1) - cz1 / km::kTileSize) * cell};
                const ImVec2 b{min.x + (cx1 / km::kTileSize - float(tx)) * cell, min.y + (float(tz + 1) - cz0 / km::kTileSize) * cell};
                draw->AddRectFilled(a, {std::max(b.x, a.x + 2.0f), std::max(b.y, a.y + 2.0f)}, withAlpha(kHighlight, 110));
                draw->AddRect(a, {std::max(b.x, a.x + 2.0f), std::max(b.y, a.y + 2.0f)}, kHighlight);
            }
    }
    const ImVec2 spawn = toScreen(info.spawn[0], info.spawn[2]);
    const float r = std::max(3.0f, cell * 0.08f);
    draw->AddLine(spawn, {spawn.x + std::sin(info.spawnHeading) * r * 2.5f, spawn.y - std::cos(info.spawnHeading) * r * 2.5f},
                  kBlack, 3.0f);
    draw->AddLine(spawn, {spawn.x + std::sin(info.spawnHeading) * r * 2.5f, spawn.y - std::cos(info.spawnHeading) * r * 2.5f},
                  kWhite, 1.5f);
    draw->AddCircleFilled(spawn, r, kWhite);
    draw->AddCircle(spawn, r, kBlack, 0, 1.5f);
    if (paint || !z.state.zones.is_null())
        drawZones(draw, info, zones, zone);
    draw->PopClipRect();

    std::optional<km::TileKey> clicked;
    if (paint && input.painting && input.hover && !locked)
        clicked = input.hover;
    else if (input.clicked && !locked && !paint) {
        if (tiles)
            clicked = input.hover;
        else
            click(doc, session, files, *input.hover);
    }

    for (const auto& [color, label] : {std::pair{kField, "No scene"}, {kPressed, "Scene"}, {kButton, "Open"},
                                       {kHighlight, "Unsaved *"}, {kError, "Orphan"}, {kCard, "Not a tile"}})
        legendItem(color, label);
    ImGui::NewLine();
    ImGui::TextDisabled(tiles ? "O spawn   [ ] portal   + add a tile"
                        : paint || !z.state.zones.is_null() ? "O spawn   [ ] portal   thick line: zone border   tint: selected zone"
                                                            : "O spawn   [ ] portal");
    return clicked;
}

void MapTiles::drawZones(ImDrawList* draw, const km::MapInfo& info, const km::Zones& zones, const std::string& selected) const
{
    const km::TileRect shown = grid_.visible();
    const float cell = grid_.view().cell, half = TileGrid::kGap * 0.5f;
    std::set<int16_t> named; // a zone's name on its first visible tile, top row first
    for (int32_t tz = shown.z1; tz >= shown.z0 - 1; --tz)
        for (int32_t tx = shown.x0 - 1; tx <= shown.x1; ++tx) {
            const km::TileKey t{tx, tz};
            if (!info.has_tile(t))
                continue;
            const int16_t in = km::zone_at(zones, t);
            const ImVec2 min = grid_.corner(t), max{min.x + cell, min.y + cell};
            if (in >= 0 && zones.list[size_t(in)].id == selected)
                draw->AddRectFilled(min, max, kZoneSelected);
            // The +x and +z edges (+z is up): each shared edge drawn once.
            if (const km::TileKey east{tx + 1, tz}; info.has_tile(east) && km::zone_at(zones, east) != in)
                draw->AddLine({max.x + half, min.y - half}, {max.x + half, max.y + half}, kZoneBorder, kZoneBorderWidth);
            if (const km::TileKey north{tx, tz + 1}; info.has_tile(north) && km::zone_at(zones, north) != in)
                draw->AddLine({min.x - half, min.y - half}, {max.x + half, min.y - half}, kZoneBorder, kZoneBorderWidth);
            if (in < 0 || tx < shown.x0 || tz < shown.z0 || cell < 24.0f || !named.insert(in).second)
                continue;
            const std::string& name = zones.list[size_t(in)].name;
            const ImVec2 text = ImGui::CalcTextSize(name.c_str());
            const ImVec2 at{min.x + 2.0f, max.y - text.y - 1.0f};
            draw->AddRectFilled({at.x - 2.0f, at.y}, {at.x + text.x + 2.0f, max.y}, IM_COL32(0, 0, 0, 200));
            draw->AddText(at, zones.list[size_t(in)].id == selected ? kHighlight : kWhite, name.c_str());
        }
}

void MapTiles::drawWarnings() const { drawWarningList("Tile scene problems ({})###TileSceneProblems", warnings_); }

void MapTiles::drawZoneWarnings() const { drawWarningList("Zone scene problems ({})###ZoneSceneProblems", zoneWarnings_); }

void MapTiles::drawWarningList(const char* header, const std::vector<std::string>& warnings)
{
    const size_t count = warnings.size();
    const std::string label = std::vformat(header, std::make_format_args(count));
    if (!ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (warnings.empty())
        ImGui::TextColored(kOkColor, "No problems");
    for (const std::string& warning : warnings) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", warning.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::TreePop();
}

void MapTiles::click(Document& doc, const MapSession& session, FileActions& files, km::TileKey t)
{
    const mapedit::MapDoc* z = session.doc();
    std::string err;
    const std::optional<km::MapInfo> info = z ? z->info(err) : std::nullopt;
    if (doc.play != PlayState::Stopped || doc.prefabEdit || !doc.project || !info || !info->has_tile(t))
        return; // undeclared cells (orphans too) ignore clicks
    toggleScene(doc, files, kast::tileScenePath(z->folder, t), "tile " + mapedit::tile_name(t));
}

void MapTiles::clickZone(Document& doc, const MapSession& session, FileActions& files, const std::string& zone)
{
    const mapedit::MapDoc* z = session.doc();
    std::string err;
    const std::optional<km::MapInfo> info = z ? z->info(err) : std::nullopt;
    if (doc.play != PlayState::Stopped || doc.prefabEdit || !doc.project || !info)
        return;
    const km::Zones zones = mapedit::map_zones(*info, z->state, z->folder);
    if (std::ranges::none_of(zones.list, [&](const km::Zone& each) { return each.id == zone; })) {
        kast::log::error(std::format("Map: maps/{} has no zone \"{}\"", z->folder, zone));
        return;
    }
    toggleScene(doc, files, kast::zoneScenePath(z->folder, mapedit::zone_scene_id(z->state, zone)), "zone " + zone);
}

kast::TileCell MapTiles::zoneScene(const Document& doc, const std::string& folder, const std::string& sceneId) const
{
    const std::optional<kast::SceneSlot> slot =
        doc.project ? findOpenScene(doc, utf8((doc.project->root / fromUtf8(kast::zoneScenePath(folder, sceneId))).lexically_normal()))
                    : std::nullopt;
    return kast::tileCell(true, zoneSceneFiles_.contains(sceneId), slot.has_value(), slot && sceneDirty(doc, *slot));
}

void MapTiles::toggleScene(Document& doc, FileActions& files, const std::string& rel, const std::string& what)
{
    const std::string file = utf8((doc.project->root / fromUtf8(rel)).lexically_normal());
    nextRefresh_ = 0.0; // counts and states again on the next draw
    if (const std::optional<kast::SceneSlot> slot = findOpenScene(doc, file)) {
        kast::log::info(std::format("Map: close {} ({})", what, rel));
        files.removeScene(*slot); // asks first if it has unsaved changes
        return;
    }
    if (std::error_code ec; std::filesystem::exists(fromUtf8(file), ec)) {
        if (std::string error; openSceneAdditive(doc, file, error))
            kast::log::info(std::format("Map: opened {} ({})", what, rel));
        else
            kast::log::error(std::format("Map: cannot open {}: {}", rel, error));
        return;
    }
    const kast::SceneSlot slot = addNewScene(doc);
    doc.scene.findScene(slot)->path = file;
    doc.sceneSaves[slot].saved.reset(); // unsaved until File > Save / Ctrl+S writes it (and makes its folder)
    updateDirty(doc);
    kast::log::info(std::format("Map: created {} for {} (unsaved: File > Save or Ctrl+S writes it)", rel, what));
}
