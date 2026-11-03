// The Map tab's Zones section (docs/editor/map-builder.html#zones): the map's zones (zones.json) listed, added,
// renamed, deleted and given flags, painted onto the tile grid (MapTiles' Paint zone mode, one undo step per stroke),
// and their zone scenes opened, closed and created. The edits are mapedit/zone_edit.h.
#include "map_panel.h"

#include "assets.h" // utf8, fromUtf8
#include "ui_common.h"

#include <mapedit/zone_edit.h>

#include <log.h>
#include <zone_scenes.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
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
    }
    strokeTiles_ += mapedit::paint_zone(info, z.state, *zone, {t});
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

    if (const auto it = std::ranges::find_if(zones.list, [&](const km::Zone& each) { return each.id == zone_; });
        it != zones.list.end())
        drawZoneFields(doc, session, files, *info, *it);
    tiles_.drawZoneWarnings();
    ImGui::SeparatorText("Tiles");
    drawTileGrid(doc, session, files, locked);
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
