// The Map tab's Zones section (docs/editor/map-builder.html#zones): the map's zones (zones.json) listed, added,
// renamed, deleted and given flags, painted onto the tile grid (MapTiles' Paint zone mode) or the terrain in the Scene
// view (one undo step per stroke; a tile whose move would split or empty a zone is refused, with why), tinted with
// their names over the terrain while the sub-tab shows, and their zone scenes opened, closed and created. The edits are mapedit/zone_edit.h.
#include "map_panel.h"

#include "assets.h" // utf8, fromUtf8
#include "ui_common.h"

#include <mapedit/zone_edit.h>

#include <log.h>
#include <zone_scenes.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace {

namespace km = kast::map;
using mapedit::Json;

// One undo step per edit of the last item: from its activation to its deactivation.
void undoScope(mapedit::MapDoc& z)
{
    if (ImGui::IsItemActivated())
        z.begin();
    if (ImGui::IsItemDeactivated())
        z.commit();
}

} // namespace

bool MapPanel::save(Document& doc, MapSession& session)
{
    const bool saved = session.save();
    if (!saved && session.doc())
        focusProblems_ = openProblems_ = true;
    // A renamed zone's open scene follows its file.
    for (const auto& [from, to] : saved && doc.project ? session.renamedZoneScenes()
                                                       : std::vector<std::pair<std::string, std::string>>())
        if (const std::optional<kast::SceneSlot> slot =
                findOpenScene(doc, utf8((doc.project->root / fromUtf8(from)).lexically_normal())))
            doc.scene.findScene(*slot)->path = utf8((doc.project->root / fromUtf8(to)).lexically_normal());
    return saved;
}

void MapPanel::paintZone(mapedit::MapDoc& z, const km::MapInfo& info, km::TileKey t)
{
    const std::optional<size_t> zone = mapedit::find_zone(z.state, zone_);
    if (!zone)
        return; // none selected, or the implicit zone (it has every tile already)
    if (!zoneStroke_) {
        z.begin();
        zoneStroke_ = true;
        strokeTiles_ = 0;
        zoneRefused_.clear();
    }
    size_t moved = 0;
    if (const std::optional<std::string> refused = mapedit::paint_zone_checked(info, z.state, z.folder, *zone, {t}, moved)) {
        const std::string why = std::format("can't paint tile {} into zone {}: {}", mapedit::tile_name(t), zone_, *refused);
        if (why != zoneRefused_) // once while the stroke stays on that tile
            kast::log::warn("Map: " + why);
        zoneRefused_ = why;
    }
    strokeTiles_ += moved;
}

bool MapPanel::zonePainting() const
{
    return tabFront_ && tab_ == Tab::Zones && tool_ == Tool::None && tiles_.mode() == MapTiles::Mode::Zones;
}

bool MapPanel::zoneInput(mapedit::MapDoc& z, const km::MapInfo& info, bool click)
{
    const std::optional<km::TileKey> under =
        hover_ ? std::optional(km::tile_of((*hover_)[0], (*hover_)[2])) : std::nullopt;
    zoneHover_ = under && info.has_tile(*under) ? under : std::nullopt;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        zoneLast_.reset();
    if (!hover_ || (!click && !zoneLast_))
        return true;
    // Every tile on the mouse's path since the last frame, so a fast drag skips none.
    const mapedit::Point to{(*hover_)[0], (*hover_)[2]};
    const mapedit::Point from = zoneLast_.value_or(to);
    const int steps = std::max(1, int(std::hypot(to[0] - from[0], to[1] - from[1]) / (km::kTileSize / 4)));
    for (int i = 0; i <= steps; ++i) {
        const float f = float(i) / float(steps);
        if (const km::TileKey t = km::tile_of(from[0] + (to[0] - from[0]) * f, from[1] + (to[1] - from[1]) * f);
            info.has_tile(t))
            paintZone(z, info, t);
    }
    zoneLast_ = to;
    return true;
}

void MapPanel::drawZoneOverlay(const GroundDraw& g, const mapedit::MapDoc& z, const km::MapInfo& info) const
{
    const km::Zones zones = mapedit::map_zones(info, z.state, z.folder);
    // Each tile tinted by its zone, in quads that follow the ground. ponytail: kSteps^2 quads a meshed tile, cheap at
    // kEditRadius; per-vertex colors on the terrain mesh if it ever shows.
    constexpr int kSteps = 8;
    constexpr float kStep = km::kTileSize / kSteps;
    const ImDrawListFlags flags = g.draw->Flags;
    g.draw->Flags &= ~ImDrawListFlags_AntiAliasedFill; // abutting quads: no feathered seams between them
    std::vector<std::array<float, 3>> sums(zones.list.size()); // tile centres' x, z and count: where the label goes
    for (const auto& [t, in] : zones.byTile) {
        const float x0 = t.x * km::kTileSize, z0 = t.z * km::kTileSize;
        sums[size_t(in)][0] += x0 + km::kTileSize / 2;
        sums[size_t(in)][1] += z0 + km::kTileSize / 2;
        sums[size_t(in)][2] += 1;
        if (!mapedit::in_edit_view(g.centre, x0 + km::kTileSize / 2, z0 + km::kTileSize / 2))
            continue;
        const ImU32 tint = kZoneTints[size_t(in) % std::size(kZoneTints)];
        for (int i = 0; i < kSteps; ++i)
            for (int j = 0; j < kSteps; ++j) {
                const float x = x0 + i * kStep, gz = z0 + j * kStep;
                const auto a = g.project(g.ground(x, gz)), b = g.project(g.ground(x + kStep, gz));
                const auto c = g.project(g.ground(x + kStep, gz + kStep)), d = g.project(g.ground(x, gz + kStep));
                if (a && b && c && d)
                    g.draw->AddQuadFilled(*a, *b, *c, *d, tint);
            }
    }
    g.draw->Flags = flags;
    if (zoneHover_) {
        const float x0 = zoneHover_->x * km::kTileSize, z0 = zoneHover_->z * km::kTileSize;
        const float x1 = x0 + km::kTileSize, z1 = z0 + km::kTileSize;
        g.line({x0, z0}, {x1, z0}, kZoneHoverLine, 3.0f);
        g.line({x1, z0}, {x1, z1}, kZoneHoverLine, 3.0f);
        g.line({x1, z1}, {x0, z1}, kZoneHoverLine, 3.0f);
        g.line({x0, z1}, {x0, z0}, kZoneHoverLine, 3.0f);
    }
    // Each zone's name on its tile nearest the zone's middle, on a card so it reads over any tint.
    for (size_t i = 0; i < zones.list.size(); ++i) {
        if (sums[i][2] == 0)
            continue;
        const float cx = sums[i][0] / sums[i][2], cz = sums[i][1] / sums[i][2];
        std::array<float, 2> best{};
        float bestDistance = INFINITY;
        for (const auto& [t, in] : zones.byTile) {
            const std::array<float, 2> c{t.x * km::kTileSize + km::kTileSize / 2, t.z * km::kTileSize + km::kTileSize / 2};
            if (const float d = std::hypot(c[0] - cx, c[1] - cz); size_t(in) == i && d < bestDistance) {
                best = c;
                bestDistance = d;
            }
        }
        const auto p = g.project(g.ground(best[0], best[1]));
        if (!p)
            continue;
        const km::Zone& zone = zones.list[i];
        const std::string label =
            zone.name.empty() || zone.name == zone.id ? zone.id : std::format("{} ({})", zone.name, zone.id);
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        const ImVec2 min(p->x - size.x / 2 - 6, p->y - size.y / 2 - 3), max(p->x + size.x / 2 + 6, p->y + size.y / 2 + 3);
        g.draw->AddRectFilled(min, max, withAlpha(kCard, 220), 4.0f);
        if (zone.id == zone_) // the zone being painted
            g.draw->AddRect(min, max, kZoneHoverLine, 4.0f, 0, 2.0f);
        g.draw->AddText(ImVec2(min.x + 6, min.y + 3), kHighlight, label.c_str());
    }
}

void MapPanel::endZoneStroke(MapSession& session)
{
    if (!zoneStroke_ || (ImGui::IsMouseDown(ImGuiMouseButton_Left) && session.doc()))
        return;
    zoneStroke_ = false;
    if (mapedit::MapDoc* z = session.doc()) {
        z->commit();
        if (strokeTiles_)
            kast::log::info(std::format("Map: painted {} tile(s) into zone {}", strokeTiles_, zone_));
    }
}

void MapPanel::drawZones(Document& doc, MapSession& session, FileActions& files, bool locked)
{
    mapedit::MapDoc* z = session.doc();
    std::string err;
    const std::optional<km::MapInfo> info = z ? z->info(err) : std::nullopt;
    if (!info || info->tiles.empty()) {
        ImGui::TextDisabled("No map open");
        return;
    }
    std::vector<std::string> problems;
    const km::Zones zones = mapedit::map_zones(*info, z->state, z->folder, &problems);
    if (std::ranges::none_of(zones.list, [&](const km::Zone& each) { return each.id == zone_; }))
        zone_ = zones.list.empty() ? std::string() : zones.list.front().id; // undone, deleted, another map
    if (z->state.zones.is_null())
        ImGui::TextWrapped("No zones.json: one implicit zone, \"%s\", of every tile. The first zone edit makes it "
                           "explicit (Save Map writes zones.json).",
                           z->folder.c_str());

    if (ImGui::Button("Add Zone")) {
        const std::string id = mapedit::unique_zone_id(z->state, "zone");
        z->edit([&](mapedit::MapState& s) { mapedit::add_zone(*info, s, z->folder, id, ""); });
        zone_ = id;
        tiles_.setMode(MapTiles::Mode::Zones);
        kast::log::info(std::format("Map: added zone {} (no tiles yet: paint some on the grid)", id));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(zone_.empty());
    if (ImGui::Button("Delete Zone"))
        deleteZone(*z, *info, zone_);
    ImGui::SameLine();
    if (ImGui::Button(tiles_.mode() == MapTiles::Mode::Zones ? "Stop painting" : "Paint tiles"))
        tiles_.setMode(tiles_.mode() == MapTiles::Mode::Zones ? MapTiles::Mode::Scenes : MapTiles::Mode::Zones);
    ImGui::EndDisabled();

    for (const km::Zone& each : zones.list) {
        size_t tiles = 0;
        for (const auto& [t, in] : zones.byTile)
            tiles += zones.list[size_t(in)].id == each.id;
        const std::string sceneId = mapedit::zone_scene_id(z->state, each.id);
        const kast::TileCell scene = tiles_.zoneScene(doc, z->folder, sceneId);
        const std::string label = std::format("{}  ({}, {} tiles{})###zone-{}", each.id, each.name, tiles,
                                              scene == kast::TileCell::Declared ? "" : ", scene", each.id);
        if (ImGui::Selectable(label.c_str(), each.id == zone_))
            zone_ = each.id;
    }
    for (const std::string& p : problems)
        ImGui::TextColored(kErrorColor, "%s", p.c_str());
    if (!zoneRefused_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("Not painted: %s", zoneRefused_.c_str());
        ImGui::PopStyleColor();
    }
    // The grid right under the list, so painting needs no scrolling; the Scene view paints too (zoneInput).
    drawTileGrid(doc, session, files, locked);

    if (const auto it = std::ranges::find_if(zones.list, [&](const km::Zone& each) { return each.id == zone_; });
        it != zones.list.end())
        drawZoneFields(doc, session, files, *info, *it);
    tiles_.drawZoneWarnings();
}

void MapPanel::drawZoneFields(Document& doc, MapSession& session, FileActions& files, const km::MapInfo& info,
                              const km::Zone& zone)
{
    mapedit::MapDoc& z = *session.doc();
    ImGui::SeparatorText(zone.id.c_str());
    // The id: applied when the field is left (a rename renames the zone scene file on save).
    if (zoneIdFor_ != zone.id) {
        zoneIdFor_ = zone.id;
        zoneIdText_ = zone.id;
        flagsText_ = zone.flags.empty() ? "" : zone.flags.dump(2);
        flagsError_.clear();
    }
    ImGui::InputText("Id", &zoneIdText_);
    if (ImGui::IsItemDeactivatedAfterEdit() && zoneIdText_ != zone.id) {
        if (!km::valid_zone_id(zoneIdText_) || mapedit::find_zone(z.state, zoneIdText_)) {
            kast::log::error(std::format("Map: zone id \"{}\": {}", zoneIdText_,
                                         km::valid_zone_id(zoneIdText_) ? "already used" : "use letters, digits, _ and - (not starting with -)"));
            zoneIdText_ = zone.id;
        } else {
            renameZone(z, info, zone.id, zoneIdText_);
            zone_ = zoneIdFor_ = zoneIdText_;
        }
    }
    std::string name = z.state.zones.is_null() ? zone.name : zone.raw.value("name", std::string());
    if (ImGui::InputText("Name", &name)) {
        Json& list = mapedit::zones_list(info, z.state, z.folder);
        const size_t i = mapedit::find_zone(z.state, zone.id).value_or(0);
        if (name.empty())
            list[i].erase("name");
        else
            list[i]["name"] = name;
    }
    undoScope(z);
    // Flags: a JSON object, applied while it parses (as an interactable's data).
    if (ImGui::InputTextMultiline("Flags (JSON object)", &flagsText_, ImVec2(0, ImGui::GetTextLineHeight() * 4))) {
        const Json parsed = Json::parse(flagsText_, nullptr, false);
        flagsError_.clear();
        Json& list = mapedit::zones_list(info, z.state, z.folder);
        const size_t i = mapedit::find_zone(z.state, zone.id).value_or(0); // explicit now: it is there
        if (flagsText_.find_first_not_of(" \t\r\n") == std::string::npos)
            list[i].erase("flags");
        else if (parsed.is_object())
            list[i]["flags"] = parsed;
        else
            flagsError_ = parsed.is_discarded() ? "not valid JSON (the last valid flags are kept)" : "must be a JSON object";
    }
    undoScope(z);
    if (!flagsError_.empty())
        ImGui::TextColored(kErrorColor, "Flags: %s", flagsError_.c_str());

    const std::string sceneId = mapedit::zone_scene_id(z.state, zone.id);
    const kast::TileCell scene = tiles_.zoneScene(doc, z.folder, sceneId);
    const std::optional<size_t> entities = tiles_.zoneEntities(sceneId);
    ImGui::Text("Scene: %s", kast::zoneScenePath(z.folder, sceneId).c_str());
    ImGui::TextDisabled("%s%s", scene == kast::TileCell::Declared ? "none" : kast::tileCellName(scene),
                        entities ? std::format(", {} entities", *entities).c_str() : "");
    const bool open = scene == kast::TileCell::Open || scene == kast::TileCell::Unsaved;
    if (ImGui::Button(open ? "Close Scene" : scene == kast::TileCell::HasScene ? "Open Scene" : "Create Scene"))
        tiles_.clickZone(doc, session, files, zone.id);
}

void MapPanel::renameZone(mapedit::MapDoc& z, const km::MapInfo& info, const std::string& from, const std::string& to)
{
    z.edit([&](mapedit::MapState& s) {
        mapedit::zones_list(info, s, z.folder);
        mapedit::rename_zone(s, *mapedit::find_zone(s, from), to);
    });
    kast::log::info(std::format("Map: renamed zone {} to {} (its scene file follows on save)", from, to));
}

void MapPanel::deleteZone(mapedit::MapDoc& z, const km::MapInfo& info, const std::string& id)
{
    std::optional<std::string> to;
    z.edit([&](mapedit::MapState& s) {
        mapedit::zones_list(info, s, z.folder);
        to = mapedit::delete_zone(s, *mapedit::find_zone(s, id));
    });
    kast::log::info(to ? std::format("Map: deleted zone {} (its tiles went to zone {})", id, *to)
                       : std::format("Map: deleted zone {} (its tiles are in no zone: paint them into one before saving)", id));
}
