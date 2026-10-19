#include "zone_panel.h"

#include "ui_common.h"

#include <zone/terrain_edit.h>
#include <zone/zone_edit.h>

#include <components.h>
#include <log.h>
#include <scene_io.h> // jsonNumber

#include <bx/math.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal, GetCurrentWindowRead
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>

namespace {

namespace km = kast::map;

constexpr const char* kUnsavedPopup = "Unsaved zone changes";
constexpr const char* kNewMapPopup = "New Map";
constexpr const char* kRemoveTilePopup = "Remove tile?";
constexpr ImVec4 kDirtyColor{1.0f, 0.8f, 0.0f, 1.0f}; // #ffcc00, the palette's highlight

// One undo step per edit of the last item: from its activation to its deactivation.
void undoScope(zone::ZoneDoc& z)
{
    if (ImGui::IsItemActivated())
        z.begin();
    if (ImGui::IsItemDeactivated())
        z.commit();
}

entt::entity firstTerrain(const Document& doc)
{
    const auto terrains = doc.scene.registry().view<kast::Terrain>();
    return terrains.empty() ? entt::null : terrains.front();
}

} // namespace

bool ZonePanel::header(const char* label, const char* section, bool defaultOpen)
{
    if (openSections_.erase(section))
        ImGui::SetNextItemOpen(true);
    return ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

bool ZonePanel::save(ZoneSession& zone)
{
    const bool saved = zone.save();
    if (!saved && zone.zone())
        focusProblems_ = openProblems_ = true;
    return saved;
}

void ZonePanel::draw(Document& doc, ZoneSession& zone)
{
    const bool locked = doc.play != PlayState::Stopped || doc.prefabEdit.has_value();
    if (locked || !zone.zone())
        tool_ = Tool::None;
    if (tool_ != Tool::None && !ImGui::GetTopMostPopupModal() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        tool_ = Tool::None;
    // The open zone's Id field edited (or undone): the Terrain entity follows, so the scene keeps naming this map.
    if (zone::ZoneDoc* z = zone.zone(); z && !locked && zone.terrainEntity() != entt::null) {
        const int id = z->state.manifest.value("id", -1);
        int& map = doc.scene.registry().get<kast::Terrain>(zone.terrainEntity()).map;
        if (id >= 0 && id <= 0xFFFF && id != map) {
            map = id;
            zone.renumber(id);
            markDirty(doc);
        }
    }
    // A blocked save: bring the tab forward. A docked tab shows a frame after focus, and not at all while the dock is
    // still being built at startup, so keep asking until it really shows.
    if (focusProblems_)
        ImGui::SetNextWindowFocus();
    if (ImGui::Begin(kWindow)) {
        const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
        if (focusProblems_ && ImGui::IsWindowFocused() && (!window->DockIsActive || window->DockTabIsVisible))
            focusProblems_ = false;
        if (!doc.project) {
            ImGui::TextWrapped("A project is required: a zone is a map in the project's maps/ folder. Open a scene that "
                               "is inside a project.");
        } else {
            if (locked) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDirtyColor);
                ImGui::TextWrapped("%s", doc.prefabEdit ? "Read-only while a prefab is open"
                                                        : "Read-only during Play (Play uses the saved map files)");
                ImGui::PopStyleColor();
            }
            ImGui::BeginDisabled(locked);
            drawTopBar(zone);
            drawTerrainEntity(doc, zone);
            drawMap(doc, zone);
            // Phases 4-5: the Terrain, Areas, Spawns, Objects and Bake sections go here, each a header(...) block.
            drawProblems(zone);
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    drawUnsavedPrompt(doc, zone);
    drawNewMapDialog(doc, zone);
    drawRemoveTilePrompt(zone);
}

void ZonePanel::drawTopBar(ZoneSession& zone)
{
    zone::ZoneDoc* z = zone.zone();
    if (z) {
        ImGui::Text("maps/%s (id %d)", z->folder.c_str(), z->state.manifest.value("id", 0));
        if (z->dirty()) {
            ImGui::SameLine();
            ImGui::TextColored(kDirtyColor, "* unsaved");
        }
    } else {
        ImGui::TextDisabled("No zone open");
    }
    ImGui::BeginDisabled(!z);
    if (ImGui::Button("Save Zone"))
        save(zone);
    ImGui::SameLine();
    ImGui::BeginDisabled(!z || !z->canUndo());
    if (ImGui::Button("Undo"))
        z->undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!z || !z->canRedo());
    if (ImGui::Button("Redo"))
        z->redo();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    // Phases 4-5: the Scene view tool buttons (toolButton) go on a row here.
    ImGui::Separator();
}

void ZonePanel::drawTerrainEntity(Document& doc, const ZoneSession& zone)
{
    const auto terrains = doc.scene.registry().view<kast::Terrain>();
    if (terrains.empty()) {
        ImGui::TextWrapped("This scene has no Terrain entity. The zone is the map a Terrain entity names: add one, or "
                           "pick or create a map below (that adds one too).");
        if (ImGui::Button("Add Terrain entity"))
            setTerrainMap(doc, zone.project().maps.empty() ? 0 : zone.project().maps.front().info.id);
        return;
    }
    if (terrains.size() > 1) {
        const kast::Name* name = doc.scene.registry().try_get<kast::Name>(terrains.front());
        ImGui::TextWrapped("%zu Terrain entities: the zone is the map of \"%s\" (the one Play streams too).",
                           terrains.size(), name ? name->value.c_str() : "");
    }
    if (!zone.error().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("The Terrain map did not load: %s", zone.error().c_str());
        ImGui::PopStyleColor();
    }
}

void ZonePanel::drawMap(Document& doc, ZoneSession& zone)
{
    if (!header("Map", "Map", true))
        return;
    const zone::ZoneDoc* z = zone.zone();
    const std::string preview = z ? z->folder : "(none)";
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("New Map...").x -
                            style.FramePadding.x * 2.0f - style.ItemSpacing.x);
    if (ImGui::BeginCombo("##map", preview.c_str())) {
        for (const zone::ProjectMap& m : zone.project().maps) {
            const std::string label = std::format("{}  (id {}, {})", m.folder, m.info.id, m.info.name);
            const bool open = z && z->folder == m.folder;
            if (ImGui::Selectable(label.c_str(), open) && !open)
                request(doc, zone, {Pending::Kind::PickMap, m.info.id});
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Map..."))
        request(doc, zone, {Pending::Kind::NewMap, 0});
    if (z && std::ranges::none_of(zone.project().maps, [&](const zone::ProjectMap& m) { return m.folder == z->folder; }))
        ImGui::TextDisabled("maps/%s is new: Save Zone writes it", z->folder.c_str());
    if (zone::ZoneDoc* open = zone.zone())
        drawManifest(*open, zone);
}

void ZonePanel::drawManifest(zone::ZoneDoc& z, const ZoneSession& zone)
{
    zone::Json& m = z.state.manifest;
    int id = m.value("id", 0);
    if (ImGui::InputInt("Id", &id))
        m["id"] = unsigned(std::clamp(id, 0, 0xFFFF)); // the parser wants an unsigned number
    undoScope(z);
    std::string name = m.value("name", std::string());
    if (ImGui::InputText("Name", &name))
        m["name"] = name;
    undoScope(z);

    const std::string material = m.value("material", std::string());
    if (ImGui::BeginCombo("Material", material.empty() ? "(default)" : material.c_str())) {
        if (ImGui::Selectable("(default)", material.empty()))
            z.edit([](zone::ZoneState& s) { s.manifest.erase("material"); });
        for (const std::string& path : zone.materials())
            if (ImGui::Selectable(path.c_str(), path == material))
                z.edit([&](zone::ZoneState& s) { s.manifest["material"] = path; });
        ImGui::EndCombo();
    }

    std::string err;
    const auto info = z.info(err);
    if (!info) {
        ImGui::TextColored(kErrorColor, "map.json: %s", err.c_str());
        return;
    }
    const auto range = km::height_range(*info);
    ImGui::Text("Height range: %.2f .. %.2f m", range[0], range[1]);

    ImGui::SeparatorText("Player spawn");
    float xz[2] = {info->spawn[0], info->spawn[2]};
    if (ImGui::InputFloat2("Position x, z", xz, "%.1f")) {
        const float y = zone::ground_y(*info, z.state, xz[0], xz[1]).value_or(info->spawn[1]);
        m["spawn"]["pos"] = zone::vec3_json({xz[0], y, xz[1]});
    }
    undoScope(z);
    float heading = info->spawnHeading * 180.0f / bx::kPi;
    if (ImGui::SliderFloat("Heading", &heading, -180.0f, 180.0f, "%.0f deg"))
        m["spawn"]["heading"] = kast::jsonNumber(heading * bx::kPi / 180.0f);
    undoScope(z);
    ImGui::Text("y %.2f", info->spawn[1]);
    ImGui::SameLine();
    toolButton("Place on terrain", Tool::SetSpawn);
    ImGui::NewLine();

    ImGui::SeparatorText("Navmesh (kast_mapbake)");
    bool nav = m.contains("nav");
    if (ImGui::Checkbox("Own nav settings", &nav))
        z.edit([&](zone::ZoneState& s) {
            if (nav)
                s.manifest["nav"] = zone::Json::object();
            else
                s.manifest.erase("nav");
        });
    if (nav && info->nav) {
        const km::NavParams p = *info->nav;
        const std::pair<const char*, float> fields[] = {{"agentRadius", p.agentRadius}, {"agentHeight", p.agentHeight},
                                                       {"maxClimb", p.maxClimb}, {"maxSlopeDeg", p.maxSlopeDeg},
                                                       {"cellSize", p.cellSize}};
        for (auto [key, value] : fields) {
            if (ImGui::InputFloat(key, &value, 0.0f, 0.0f, "%.2f"))
                m["nav"][key] = kast::jsonNumber(std::max(0.0f, value));
            undoScope(z);
        }
    }

    ImGui::SeparatorText("Tiles");
    drawTileGrid(z, *info);
}

void ZonePanel::drawTileGrid(zone::ZoneDoc& z, const km::MapInfo& info)
{
    const zone::TileSet tiles = zone::declared_tiles(info);
    int32_t x0 = INT32_MAX, z0 = INT32_MAX, x1 = INT32_MIN, z1 = INT32_MIN;
    for (km::TileKey t : tiles) {
        x0 = std::min(x0, t.x - 1);
        z0 = std::min(z0, t.z - 1);
        x1 = std::max(x1, t.x + 1);
        z1 = std::max(z1, t.z + 1);
    }
    ImGui::TextDisabled("Click a free cell to add a tile, a tile to remove it. +z is up.");
    const float cell = ImGui::GetFrameHeight() * 1.6f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));
    for (int32_t tz = z1; tz >= z0; --tz) { // +z up, as seen from above with the default camera
        for (int32_t tx = x0; tx <= x1; ++tx) {
            const km::TileKey t{tx, tz};
            const bool declared = tiles.contains(t);
            ImGui::PushID(int(tx * 7919 + tz));
            if (!declared)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::Button(declared ? zone::tile_name(t).c_str() : "+", ImVec2(cell, cell))) {
                if (!declared)
                    z.edit([&](zone::ZoneState& s) { zone::add_tile(info, s, t); });
                else if (tiles.size() > 1)
                    removeTile_ = t;
            }
            if (!declared)
                ImGui::PopStyleColor();
            ImGui::PopID();
            if (tx < x1)
                ImGui::SameLine();
        }
    }
    ImGui::PopStyleVar();
}

void ZonePanel::drawProblems(const ZoneSession& zone)
{
    // New problems (a load or a blocked save): expand the section.
    if (zone.problems().size() != shownProblems_)
        openProblems_ = openProblems_ || !zone.problems().empty();
    shownProblems_ = zone.problems().size();
    if (std::exchange(openProblems_, false))
        ImGui::SetNextItemOpen(true);
    const std::string label = std::format("Problems ({})###Problems", zone.problems().size());
    if (!header(label.c_str(), "Problems"))
        return;
    if (!zone.zone())
        ImGui::TextDisabled("No zone open");
    else if (zone.problems().empty())
        ImGui::TextColored(kOkColor, "No problems");
    for (const zone::ZoneError& e : zone.problems()) {
        ImGui::TextColored(kErrorColor, "%s:", e.file.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", e.message.c_str());
    }
}

void ZonePanel::toolButton(const char* label, Tool tool)
{
    const bool active = tool_ == tool;
    if (active)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label))
        tool_ = active ? Tool::None : tool;
    if (active)
        ImGui::PopStyleColor();
    ImGui::SameLine();
}

void ZonePanel::request(Document& doc, ZoneSession& zone, Pending pending)
{
    if (zone.dirty()) {
        pending_ = pending;
        askUnsaved_ = true;
        return;
    }
    run(doc, zone, pending);
}

void ZonePanel::run(Document& doc, ZoneSession& zone, const Pending& pending)
{
    tool_ = Tool::None;
    switch (pending.kind) {
    case Pending::Kind::PickMap:
        setTerrainMap(doc, pending.map); // the session loads it next frame
        break;
    case Pending::Kind::NewMap:
        newFolder_.clear();
        newName_.clear();
        newError_.clear();
        newId_ = zone::next_map_id(zone.project());
        openNewMap_ = true;
        break;
    case Pending::Kind::None: break;
    }
}

void ZonePanel::setTerrainMap(Document& doc, int id)
{
    entt::entity terrain = firstTerrain(doc);
    if (terrain == entt::null) {
        terrain = doc.scene.create("Terrain");
        doc.scene.registry().emplace<kast::Terrain>(terrain);
        doc.selected = terrain;
        kast::log::info("Zone: added a Terrain entity");
    }
    doc.scene.registry().get<kast::Terrain>(terrain).map = id;
    markDirty(doc);
}

void ZonePanel::drawUnsavedPrompt(Document& doc, ZoneSession& zone)
{
    if (std::exchange(askUnsaved_, false))
        ImGui::OpenPopup(kUnsavedPopup);
    if (!ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("maps/%s has unsaved changes. Save them before changing the map?",
                zone.zone() ? zone.zone()->folder.c_str() : "");
    if (ImGui::Button("Save")) {
        ImGui::CloseCurrentPopup();
        if (save(zone)) // blocked: the problems show, the edits stay
            run(doc, zone, std::exchange(pending_, {}));
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save")) {
        ImGui::CloseCurrentPopup();
        zone.reload(); // the edits go even if the New Map dialog is cancelled
        run(doc, zone, std::exchange(pending_, {}));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        pending_ = {};
    }
    ImGui::EndPopup();
}

void ZonePanel::drawNewMapDialog(Document& doc, ZoneSession& zone)
{
    if (std::exchange(openNewMap_, false))
        ImGui::OpenPopup(kNewMapPopup);
    if (!ImGui::BeginPopupModal(kNewMapPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::InputText("Folder", &newFolder_);
    ImGui::InputText("Name", &newName_);
    ImGui::InputInt("Id", &newId_);
    ImGui::InputInt("Tiles X", &newWidth_);
    ImGui::InputInt("Tiles Z", &newDepth_);
    newId_ = std::clamp(newId_, 0, 0xFFFF);
    newWidth_ = std::clamp(newWidth_, 1, 8);
    newDepth_ = std::clamp(newDepth_, 1, 8);
    const bool used = std::ranges::any_of(zone.project().maps, [&](const zone::ProjectMap& m) { return m.info.id == newId_; });
    if (used)
        ImGui::TextColored(kErrorColor, "Id %d is already used", newId_);
    ImGui::BeginDisabled(used || newFolder_.empty());
    if (ImGui::Button("Create")) {
        if (zone.newMap(newFolder_, newName_.empty() ? newFolder_ : newName_, uint16_t(newId_), newWidth_, newDepth_,
                        newError_)) {
            setTerrainMap(doc, newId_);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    if (!newError_.empty())
        ImGui::TextColored(kErrorColor, "%s", newError_.c_str());
    ImGui::EndPopup();
}

void ZonePanel::drawRemoveTilePrompt(ZoneSession& zone)
{
    if (removeTile_ && !ImGui::IsPopupOpen(kRemoveTilePopup))
        ImGui::OpenPopup(kRemoveTilePopup);
    if (!ImGui::BeginPopupModal(kRemoveTilePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    zone::ZoneDoc* z = zone.zone();
    const km::TileKey t = removeTile_.value_or(km::TileKey{});
    if (!z) { // the zone went away (a scene change) while asking
        removeTile_.reset();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("Remove tile %s from maps/%s?", zone::tile_name(t).c_str(), z->folder.c_str());
    const std::vector<std::string> files = zone::tile_files(zone.project(), z->folder, t);
    if (files.empty())
        ImGui::TextDisabled("It has no files on disk.");
    else
        ImGui::Text("These files are deleted (to the Recycle Bin) when you save:");
    for (const std::string& f : files)
        ImGui::BulletText("%s", f.c_str());
    if (const auto it = z->state.spawns.find(t);
        it != z->state.spawns.end() && !it->second.value("spawns", zone::Json::array()).empty()) {
        ImGui::Text("Its spawns are lost:");
        for (const zone::Json& s : it->second["spawns"])
            ImGui::BulletText("%s (%s)", s.value("id", "").c_str(), s.value("npc", "").c_str());
    }
    if (ImGui::Button("Remove")) {
        std::string err;
        if (const auto info = z->info(err))
            z->edit([&](zone::ZoneState& s) { zone::remove_tile(*info, s, t); });
        removeTile_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        removeTile_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
