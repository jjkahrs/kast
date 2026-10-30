#include "map_panel.h"

#include "assets.h" // utf8, fromUtf8
#include "ui_common.h"

#include <zone/terrain_edit.h>

#include <components.h>
#include <files.h> // readFile
#include <log.h>
#include <project.h> // toProjectPath

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace {

namespace km = kast::map;

constexpr double kRefreshSeconds = 0.5; // the scene files and counts are read again this often (file changes)
constexpr int kThumbnailSize = 64;
constexpr float kGap = 2.0f; // pixels between cells
constexpr ImU32 kButton = IM_COL32(255, 170, 0, 255);    // #ffaa00: an open tile scene
constexpr ImU32 kHighlight = IM_COL32(255, 204, 0, 255); // #ffcc00: unsaved, the hovered cell, portals
constexpr ImU32 kDarken = IM_COL32(255, 153, 0, 255);    // #ff9900: a tile scene on disk
constexpr ImU32 kCard = IM_COL32(49, 49, 49, 255);       // #313131: an undeclared cell
constexpr ImU32 kField = IM_COL32(69, 69, 69, 255);      // #454545: a declared tile without a scene
constexpr ImU32 kFieldHighlight = IM_COL32(129, 129, 129, 255); // #818181: an undeclared cell's label
constexpr ImU32 kWarning = IM_COL32(255, 115, 102, 255); // kErrorColor: an orphan scene
constexpr ImU32 kBlack = IM_COL32(0, 0, 0, 255);
// Not ImGui's text color, so darkenTextOnAccent leaves cell labels on an open cell's accent tint white.
constexpr ImU32 kWhite = IM_COL32(250, 250, 250, 255);

ImU32 borderColor(kast::TileCell cell)
{
    switch (cell) {
    case kast::TileCell::Declared: return kField;
    case kast::TileCell::HasScene: return kDarken;
    case kast::TileCell::Open: return kButton;
    case kast::TileCell::Unsaved: return kHighlight;
    case kast::TileCell::Orphan: return kWarning;
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

std::filesystem::path MapPanel::tileFile(const std::filesystem::path& root, const std::string& folder, km::TileKey t) const
{
    return (root / fromUtf8(kast::tileScenePath(folder, t))).lexically_normal();
}

void MapPanel::draw(Document& doc, const ZoneSession& zone, FileActions& files)
{
    const bool locked = doc.play != PlayState::Stopped || doc.prefabEdit.has_value();
    const zone::ZoneDoc* z = zone.zone();
    std::string err;
    const std::optional<km::MapInfo> info = z ? z->info(err) : std::nullopt;
    const bool shown = doc.project && zone.terrainEntity() != entt::null && info && !info->tiles.empty();
    // Also while the tab is behind another: its warnings reach the Console. In Play the open scenes are the streamed
    // ones; in prefab edit mode doc.scene is the prefab, so the last state stays.
    if (shown && !doc.prefabEdit &&
        (z->folder != folder_ || doc.sceneGeneration != generation_ || ImGui::GetTime() >= nextRefresh_))
        refresh(doc, *z, *info);
    if (ImGui::Begin(kWindow)) {
        if (!doc.project) {
            ImGui::TextWrapped("A project is required: a zone is a map in the project's maps/ folder. Open a scene that "
                               "is inside a project.");
        } else if (zone.terrainEntity() == entt::null) {
            ImGui::TextWrapped("This scene has no Terrain entity. The Map tab shows the zone a Terrain entity names: add "
                               "one, or pick or create a map in the Zone tab.");
        } else if (!shown) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextWrapped("The Terrain map did not load: %s",
                               !zone.error().empty() ? zone.error().c_str()
                               : z                   ? std::format("maps/{}/map.json: {}", z->folder, err).c_str()
                                                     : "no zone open");
            ImGui::PopStyleColor();
        } else {
            if (locked) {
                ImGui::PushStyleColor(ImGuiCol_Text, kHighlight);
                ImGui::TextWrapped("%s", doc.prefabEdit ? "Read-only while a prefab is open"
                                                        : "Read-only during Play: tile scenes stream around the player");
                ImGui::PopStyleColor();
            }
            syncThumbnails(*z, *info);
            drawGrid(doc, zone, files, *info, locked);
        }
    }
    ImGui::End();
}

void MapPanel::refresh(const Document& doc, const zone::ZoneDoc& z, const km::MapInfo& info)
{
    const std::filesystem::path& root = doc.project->root;
    const kast::TileSceneFiles listing = kast::listTileScenes(root / "maps" / fromUtf8(z.folder));
    sceneFiles_ = {listing.tiles.begin(), listing.tiles.end()};
    if (z.folder != folder_)
        kast::log::info(std::format("Map: maps/{} ({} tiles, {} tile scenes)", z.folder,
                                    zone::declared_tiles(info).size(), sceneFiles_.size()));
    folder_ = z.folder;
    generation_ = doc.sceneGeneration;
    nextRefresh_ = ImGui::GetTime() + kRefreshSeconds;

    // The tile scenes: their files, and open ones not saved yet (a created cell).
    std::set<km::TileKey, zone::TileLess> tiles = sceneFiles_;
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

    const zone::TileSet declared = zone::declared_tiles(info);
    entities_.clear();
    warnings_.clear();
    for (const km::TileKey t : tiles) {
        const std::string rel = kast::tileScenePath(z.folder, t);
        const std::filesystem::path file = tileFile(root, z.folder, t);
        std::vector<kast::TileSceneRoot> roots;
        if (const std::optional<kast::SceneSlot> slot = findOpenScene(doc, utf8(file))) {
            entities_[t] = perScene[*slot];
            for (const entt::entity entity : doc.scene.children(entt::null))
                if (doc.scene.sceneOf(entity) == *slot) {
                    const bx::Vec3& p = doc.scene.registry().get<kast::Transform>(entity).position; // a root's is world
                    roots.push_back({doc.scene.registry().get<kast::Name>(entity).value, {p.x, p.z}});
                }
        } else {
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
                warnings_.push_back(std::format("{}: {}", rel, summary.error));
                continue;
            }
            entities_[t] = summary.summary->entities;
            roots = summary.summary->roots;
        }
        if (!declared.contains(t))
            warnings_.push_back(std::format("{} is an orphan: tile {} is not a zone tile (declare it in the Zone tab, "
                                            "or move its entities into another tile scene)",
                                            rel, zone::tile_name(t)));
        for (const kast::TileSceneRoot& r : roots)
            if (const std::optional<km::TileKey> at = kast::outsideTile(t, r.xz[0], r.xz[1]))
                warnings_.push_back(std::format("{}: root entity '{}' at ({:.0f}, {:.0f}) is in tile {}, outside tile {}",
                                                rel, r.name, r.xz[0], r.xz[1], zone::tile_name(*at), zone::tile_name(t)));
    }
    for (const std::string& name : listing.badNames)
        warnings_.push_back(std::format("maps/{}/scenes/{}: not a tile scene name (<tx>_<tz>.scene), ignored", z.folder, name));

    std::set<std::string> shown(warnings_.begin(), warnings_.end());
    for (const std::string& warning : warnings_)
        if (!logged_.contains(warning))
            kast::log::warn("Map: " + warning);
    logged_ = std::move(shown);
}

void MapPanel::syncThumbnails(const zone::ZoneDoc& z, const km::MapInfo& info)
{
    const std::string colors = z.state.manifest.value("terrainColors", zone::Json()).dump();
    if (colors != thumbnailColors_)
        shutdown(); // new colors: every thumbnail again (another map's tiles have their own heights buffers)
    thumbnailColors_ = colors;
    std::string err;
    const std::optional<zone::Gradient> gradient =
        z.state.manifest.contains("terrainColors") ? zone::parse_gradient(z.state.manifest, err) : std::nullopt;
    const zone::TileSet declared = zone::declared_tiles(info);
    std::erase_if(thumbnails_, [&](auto& entry) {
        if (declared.contains(entry.first))
            return false;
        bgfx::destroy(entry.second.texture);
        return true;
    });
    for (const km::TileKey t : declared) {
        const auto it = z.state.heights.find(t);
        const zone::HeightsPtr heights = it == z.state.heights.end() ? nullptr : it->second;
        Thumbnail& thumbnail = thumbnails_[t];
        if (bgfx::isValid(thumbnail.texture) && thumbnail.heights == heights)
            continue; // a Zone tab edit makes a new buffer (copy on write)
        if (bgfx::isValid(thumbnail.texture))
            bgfx::destroy(thumbnail.texture);
        const zone::TerrainImage image = zone::tile_thumbnail(z.state, t, gradient, kThumbnailSize);
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

void MapPanel::shutdown()
{
    for (auto& [tile, thumbnail] : thumbnails_)
        if (bgfx::isValid(thumbnail.texture))
            bgfx::destroy(thumbnail.texture);
    thumbnails_.clear();
}

void MapPanel::drawGrid(Document& doc, const ZoneSession& zone, FileActions& files, const km::MapInfo& info, bool locked)
{
    const zone::ZoneDoc& z = *zone.zone();
    const zone::TileSet declared = zone::declared_tiles(info);
    km::TileKey first = *declared.begin(), last = first; // the bounds' bottom-left and top-right corners
    for (const km::TileKey t : declared) {
        first = {std::min(first.x, t.x), std::min(first.z, t.z)};
        last = {std::max(last.x, t.x), std::max(last.z, t.z)};
    }
    // ponytail: the whole bounds at once, cells shrink to fit (no labels below 24 px); zoom and pan
    // when big maps need it.
    const int grid = std::max({kMinGrid, last.x - first.x + 1, last.z - first.z + 1});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("maps/%s: click a tile to open, close or create its scene. +z is up.", z.folder.c_str());
    ImGui::PopStyleColor();
    const float cell = std::clamp(std::floor((ImGui::GetContentRegionAvail().x - kGap * float(grid - 1)) / float(grid)), 4.0f, 96.0f);
    const float pitch = cell + kGap;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tiles", {pitch * grid - kGap, pitch * grid - kGap});
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const auto cellMin = [&](km::TileKey t) {
        return ImVec2(origin.x + float(t.x - first.x) * pitch, origin.y + float(grid - 1 - (t.z - first.z)) * pitch);
    };
    // A map position on screen: inside its tile's cell (the gaps between cells are not ground).
    const auto toScreen = [&](float x, float zz) {
        const km::TileKey t = km::tile_of(x, zz);
        const ImVec2 min = cellMin(t);
        return ImVec2(min.x + (x / km::kTileSize - float(t.x)) * cell, min.y + (float(t.z + 1) - zz / km::kTileSize) * cell);
    };

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::filesystem::path& root = doc.project->root;
    std::optional<km::TileKey> hover;
    for (int row = 0; row < grid; ++row)
        for (int col = 0; col < grid; ++col) {
            const km::TileKey t{first.x + col, first.z + grid - 1 - row};
            const std::optional<kast::SceneSlot> slot = findOpenScene(doc, utf8(tileFile(root, z.folder, t)));
            const kast::TileCell state = kast::tileCell(declared.contains(t), sceneFiles_.contains(t), slot.has_value(),
                                                        slot && sceneDirty(doc, *slot));
            const ImVec2 min = cellMin(t), max{min.x + cell, min.y + cell};
            const auto thumbnail = thumbnails_.find(t);
            if (thumbnail != thumbnails_.end() && bgfx::isValid(thumbnail->second.texture))
                draw->AddImage(static_cast<ImTextureID>(thumbnail->second.texture.idx), min, max);
            else
                draw->AddRectFilled(min, max, state == kast::TileCell::Declared ? kField : kCard);
            if (state == kast::TileCell::Open || state == kast::TileCell::Unsaved) // open in the editor: tinted
                draw->AddRectFilled(min, max, (borderColor(state) & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 120));
            const std::string label = zone::tile_name(t) + (state == kast::TileCell::Unsaved ? "*" : "");
            const ImVec2 text = ImGui::CalcTextSize(label.c_str());
            const bool fits = cell >= 24.0f; // the smallest cell labels and counts were laid out for
            if (fits && state != kast::TileCell::Undeclared)
                draw->AddRectFilled(min, {min.x + text.x + 4.0f, min.y + text.y}, IM_COL32(0, 0, 0, 160));
            if (fits)
                draw->AddText({min.x + 2.0f, min.y}, state == kast::TileCell::Undeclared ? kFieldHighlight
                                                 : state == kast::TileCell::Orphan  ? kWarning
                                                                                     : kWhite,
                          label.c_str());
            if (const auto count = entities_.find(t); fits && count != entities_.end()) {
                const std::string n = std::to_string(count->second);
                const ImVec2 size = ImGui::CalcTextSize(n.c_str());
                const ImVec2 at{max.x - size.x - 2.0f, max.y - size.y};
                draw->AddRectFilled({at.x - 2.0f, at.y}, max, IM_COL32(0, 0, 0, 160));
                draw->AddText(at, kWhite, n.c_str());
            }
            draw->AddRect(min, max, borderColor(state), 0.0f, 0, borderWidth(state));
            const ImVec2 mouse = ImGui::GetMousePos();
            if (hovered && mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y) {
                hover = t;
                ImGui::SetTooltip("%s\n%s%s", kast::tileScenePath(z.folder, t).c_str(), kast::tileCellName(state),
                                  entities_.contains(t) ? std::format("\n{} entities", entities_.at(t)).c_str() : "");
                if (state != kast::TileCell::Undeclared && state != kast::TileCell::Orphan)
                    draw->AddRect({min.x - 1.0f, min.y - 1.0f}, {max.x + 1.0f, max.y + 1.0f}, kHighlight, 0.0f, 0, 1.0f);
            }
        }

    // The portal areas (clipped to each cell they cross) and the player spawn with its heading.
    const ImVec2 gridMax{origin.x + pitch * grid - kGap, origin.y + pitch * grid - kGap};
    draw->PushClipRect(origin, gridMax, true);
    std::string err;
    std::optional<km::Areas> areas = z.state.areas.is_null() ? std::nullopt : km::parse_areas(nlohmann::json(z.state.areas), err);
    for (const km::Area& area : areas ? areas->list : std::vector<km::Area>()) {
        if (!area.portal)
            continue;
        const auto [x0, z0, x1, z1] = area.bounds;
        for (int32_t tz = km::tile_of(x0, z0).z; tz <= km::tile_of(x1, z1).z; ++tz)
            for (int32_t tx = km::tile_of(x0, z0).x; tx <= km::tile_of(x1, z1).x; ++tx) {
                const float cx0 = std::max(x0, float(tx) * km::kTileSize), cx1 = std::min(x1, float(tx + 1) * km::kTileSize);
                const float cz0 = std::max(z0, float(tz) * km::kTileSize), cz1 = std::min(z1, float(tz + 1) * km::kTileSize);
                const ImVec2 min = cellMin({tx, tz});
                const ImVec2 a{min.x + (cx0 / km::kTileSize - float(tx)) * cell, min.y + (float(tz + 1) - cz1 / km::kTileSize) * cell};
                const ImVec2 b{min.x + (cx1 / km::kTileSize - float(tx)) * cell, min.y + (float(tz + 1) - cz0 / km::kTileSize) * cell};
                draw->AddRectFilled(a, {std::max(b.x, a.x + 2.0f), std::max(b.y, a.y + 2.0f)}, IM_COL32(255, 204, 0, 110));
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
    draw->PopClipRect();

    if (clicked && hover && !locked)
        click(doc, zone, files, *hover);

    for (const auto& [color, label] : {std::pair{kField, "No scene"}, {kDarken, "Scene"}, {kButton, "Open"},
                                       {kHighlight, "Unsaved *"}, {kWarning, "Orphan"}, {kCard, "Not a tile"}})
        legendItem(color, label);
    ImGui::NewLine();
    ImGui::TextDisabled("O spawn   [ ] portal");
    const std::string header = std::format("Problems ({})###MapProblems", warnings_.size());
    if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (warnings_.empty())
            ImGui::TextColored(kOkColor, "No problems");
        for (const std::string& warning : warnings_) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
        }
    }
}

void MapPanel::click(Document& doc, const ZoneSession& zone, FileActions& files, km::TileKey t)
{
    const zone::ZoneDoc* z = zone.zone();
    std::string err;
    const std::optional<km::MapInfo> info = z ? z->info(err) : std::nullopt;
    if (doc.play != PlayState::Stopped || doc.prefabEdit || !doc.project || !info || !info->has_tile(t))
        return; // undeclared cells (orphans too) ignore clicks
    const std::string rel = kast::tileScenePath(z->folder, t);
    const std::string file = utf8(tileFile(doc.project->root, z->folder, t));
    nextRefresh_ = 0.0; // counts and states again on the next draw
    if (const std::optional<kast::SceneSlot> slot = findOpenScene(doc, file)) {
        kast::log::info(std::format("Map: close tile {} ({})", zone::tile_name(t), rel));
        files.removeScene(*slot); // asks first if it has unsaved changes
        return;
    }
    if (std::error_code ec; std::filesystem::exists(fromUtf8(file), ec)) {
        if (std::string error; openSceneAdditive(doc, file, error))
            kast::log::info(std::format("Map: opened tile {} ({})", zone::tile_name(t), rel));
        else
            kast::log::error(std::format("Map: cannot open {}: {}", rel, error));
        return;
    }
    const kast::SceneSlot slot = addNewScene(doc);
    doc.scene.findScene(slot)->path = file;
    doc.sceneSaves[slot].saved.reset(); // unsaved until File > Save / Ctrl+S writes it (and makes scenes/)
    updateDirty(doc);
    kast::log::info(std::format("Map: created {} for tile {} (unsaved: File > Save or Ctrl+S writes it)", rel,
                                zone::tile_name(t)));
}
