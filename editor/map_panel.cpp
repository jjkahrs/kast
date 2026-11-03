#include "map_panel.h"

#include "assets.h" // fromUtf8
#include "editor_settings.h"
#include "map_cli.h" // frameMap
#include "ui_common.h"

#include <mapedit/terrain_edit.h>
#include <mapedit/map_edit.h>
#include <mapedit/zone_edit.h>

#include <components.h>
#include <log.h>
#include <math3d.h>   // screenRay
#include <scene_io.h> // jsonNumber

#include <bx/math.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal, GetCurrentWindowRead
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>

namespace {

namespace km = kast::map;

constexpr const char* kUnsavedPopup = "Unsaved map changes";
constexpr const char* kNewMapPopup = "New Map";
constexpr const char* kReplacePopup = "Replace map link?";
constexpr std::pair<const char*, MapPanel::Tab> kTabs[] = {
    {"Setup", MapPanel::Tab::Setup},     {"Zones", MapPanel::Tab::Zones}, {"Terrain", MapPanel::Tab::Terrain},
    {"Objects", MapPanel::Tab::Objects}, {"Bake", MapPanel::Tab::Bake}};
// --map-section's section names: the sub-tab each is in.
constexpr std::pair<const char*, MapPanel::Tab> kSectionTabs[] = {
    {"Map", MapPanel::Tab::Setup},     {"Tiles", MapPanel::Tab::Setup},    {"Areas", MapPanel::Tab::Objects},
    {"Spawns", MapPanel::Tab::Objects}, {"Objects", MapPanel::Tab::Objects}};
constexpr const char* kRemoveTilePopup = "Remove tile?";
constexpr ImVec4 kDirtyColor = toVec4(kHighlight);
constexpr ImU32 kBrushColor = kHighlight; // the brush ring
constexpr ImVec4 kToolIdle = toVec4(kField); // an idle tool button
constexpr ImVec4 kToolActive = toVec4(kHighlight); // the active tool
constexpr float kPickDistance = 10000.0f; // metres: past the far corner of any map in view
constexpr std::pair<const char*, MapPanel::Tool> kToolNames[] = {
    {"Raise", MapPanel::Tool::Raise},           {"Lower", MapPanel::Tool::Lower},
    {"Flatten", MapPanel::Tool::Flatten},       {"AreaRect", MapPanel::Tool::AreaRect},
    {"AreaPolygon", MapPanel::Tool::AreaPolygon}, {"PlaceSpawn", MapPanel::Tool::PlaceSpawn},
    {"PathPoints", MapPanel::Tool::PathPoints}, {"SetSpawn", MapPanel::Tool::SetSpawn},
    {"PlaceObject", MapPanel::Tool::PlaceObject}, {"Scatter", MapPanel::Tool::Scatter}};

// One undo step per edit of the last item: from its activation to its deactivation.
void undoScope(mapedit::MapDoc& z)
{
    if (ImGui::IsItemActivated())
        z.begin();
    if (ImGui::IsItemDeactivated())
        z.commit();
}

// A new instance map's exit: the first other world map of the project, at its spawn (map 0 at the origin if none:
// the Problems section then says why).
km::Portal defaultExit(const MapSession& session, const std::string& folder)
{
    for (const mapedit::ProjectMap& m : session.project().maps)
        if (m.folder != folder && !m.info.instanced())
            return {m.info.id, m.info.spawn, m.info.spawnHeading};
    return {};
}

entt::entity firstTerrain(const Document& doc)
{
    const auto terrains = doc.scene.registry().view<kast::Terrain>();
    return terrains.empty() ? entt::null : terrains.front();
}

// The New Map wizard's folder for a map name: lower case letters and digits, '-' and '_' kept, anything else '_'.
std::string folderFor(const std::string& name)
{
    std::string folder;
    for (const unsigned char c : name)
        folder += std::isalnum(c) && c < 0x80 ? char(std::tolower(c)) : c == '-' ? '-' : '_';
    return folder.substr(std::min(folder.find_first_not_of('-'), folder.size())); // not starting with '-'
}

// The sub-tab whose top bar has tool's button.
MapPanel::Tab tabOf(MapPanel::Tool tool)
{
    switch (tool) {
    case MapPanel::Tool::Raise:
    case MapPanel::Tool::Lower:
    case MapPanel::Tool::Flatten: return MapPanel::Tab::Terrain;
    case MapPanel::Tool::None:
    case MapPanel::Tool::SetSpawn: return MapPanel::Tab::Setup;
    default: return MapPanel::Tab::Objects;
    }
}

template <typename T>
bool parseNumber(std::string_view text, T& out)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

} // namespace

MapPanel::MapPanel(std::filesystem::path settingsFile)
    : settingsFile_(std::move(settingsFile)), mapbake_(editorSetting(loadEditorSettings(settingsFile_), "mapbake"))
{
}

bool MapPanel::header(const char* label, const char* section, bool defaultOpen)
{
    if (showSections_ && sectionsShown_.insert(section).second)
        ImGui::SetNextItemOpen(showSections_->contains(section));
    return ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

void MapPanel::draw(Document& doc, MapSession& session, FileActions& files)
{
    const bool locked = doc.play != PlayState::Stopped || doc.prefabEdit.has_value();
    if (locked || !session.doc())
        tool_ = Tool::None;
    if (tool_ != Tool::None && !ImGui::GetTopMostPopupModal() && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (!polygon_.empty() || rect_) { // the area being drawn first
            polygon_.clear();
            rect_.reset();
        } else {
            tool_ = Tool::None;
        }
    }
    if (tool_ != Tool::AreaPolygon)
        polygon_.clear();
    if (tool_ != Tool::AreaRect)
        rect_.reset();
    endStroke(session);
    endZoneStroke(session);
    checkSelection(session.doc());
    defaultPrefabs(session);
    pollPngDialog(session);
    pollBake();
    tiles_.update(doc, session); // also while the tab is behind another: tile scene warnings reach the Console
    // The open map's Id field edited (or undone): the Terrain entity follows, so the scene keeps naming this map.
    if (mapedit::MapDoc* z = session.doc(); z && !locked && session.terrainEntity() != entt::null) {
        const int id = z->state.manifest.value("id", -1);
        int& map = doc.scene.registry().get<kast::Terrain>(session.terrainEntity()).map;
        if (id >= 0 && id <= 0xFFFF && id != map) {
            map = id;
            session.renumber(id);
            markDirty(doc);
        }
    }
    // A blocked save: bring the tab forward. A docked tab shows a frame after focus, and not at all while the dock is
    // still being built at startup, so keep asking until it really shows.
    if (focusProblems_)
        ImGui::SetNextWindowFocus();
    tabFront_ = false;
    if (ImGui::Begin(kWindow)) {
        const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
        tabFront_ = !window->DockIsActive || window->DockTabIsVisible;
        if (focusProblems_ && ImGui::IsWindowFocused() && tabFront_)
            focusProblems_ = false;
        if (!doc.project) {
            ImGui::TextWrapped("A project is required: the map is one of the project's maps/ folders. Open a scene that "
                               "is inside a project.");
        } else {
            if (locked) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDirtyColor);
                ImGui::TextWrapped("%s", doc.prefabEdit ? "Read-only while a prefab is open"
                                                        : "Read-only during Play (Play uses the saved map files; tile scenes "
                                                          "stream around the player)");
                ImGui::PopStyleColor();
            }
            ImGui::BeginDisabled(locked);
            drawTopBar(doc, session);
            drawTerrainEntity(doc, session);
            drawProblems(session);
            drawTabs(doc, session, files, locked);
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    zoneOverlay_ = tabFront_ && tab_ == Tab::Zones && session.doc();
    drawUnsavedPrompt(doc, session);
    drawNewMapDialog(doc, session, files);
    drawRemoveTilePrompt(session);
    drawHeightRangePrompt(session);
}

std::optional<MapPanel::Tool> MapPanel::toolNamed(std::string_view name)
{
    for (const auto& [n, tool] : kToolNames)
        if (name == n)
            return tool;
    return std::nullopt;
}

void MapPanel::showSections(const std::vector<std::string>& names)
{
    showSections_ = std::set(names.begin(), names.end());
    for (const std::string& name : names) {
        for (const auto& [n, tab] : kTabs)
            if (name == n)
                selectTab(tab);
        for (const auto& [n, tab] : kSectionTabs)
            if (name == n)
                selectTab(tab);
    }
}

void MapPanel::selectTab(Tab tab)
{
    followTab(tab);
}

void MapPanel::followTab(Tab tab)
{
    if (tab == tab_)
        return;
    tab_ = tab;
    tool_ = tab == Tab::Terrain ? Tool::Raise : Tool::None;
    if (tab == Tab::Zones)
        tiles_.setMode(MapTiles::Mode::Zones);
    else if (tiles_.mode() == MapTiles::Mode::Zones)
        tiles_.setMode(MapTiles::Mode::Scenes);
    if (tab == Tab::Zones || tab == Tab::Terrain)
        kast::log::info(tab == Tab::Zones ? "Map: Zones tab: the tile grid paints zones"
                                          : "Map: Terrain tab: Raise tool (left drag in the Scene view)");
}

void MapPanel::setTool(Tool tool)
{
    selectTab(tabOf(tool));
    tool_ = tool;
}

void MapPanel::drawTabs(Document& doc, MapSession& session, FileActions& files, bool locked)
{
    // Tab-styled buttons that wrap to a second row, so every label stays whole in the narrow default dock (an ImGui
    // tab bar shrinks or scrolls them instead): the selected one grey with accent text, the others card grey.
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, style.TabRounding);
    for (const auto& [name, t] : kTabs) {
        const bool selected = t == tab_;
        if (t != kTabs[0].second) {
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(name).x + style.FramePadding.x * 2.0f)
                ImGui::NewLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, toVec4(selected ? kField : kCard));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(selected ? kField : kRowHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(kField));
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(selected ? kAccent : kTextSecondary));
        if (ImGui::Button(name))
            followTab(t);
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar();
    ImGui::Separator();
    switch (tab_) {
    case Tab::Setup:
        drawMap(doc, session);
        drawTiles(doc, session, files, locked);
        break;
    case Tab::Zones: drawZones(doc, session, files, locked); break;
    case Tab::Terrain: drawTerrain(session); break;
    case Tab::Objects:
        drawAreas(session);
        drawSpawns(session);
        drawObjects(session);
        break;
    case Tab::Bake: drawBake(session); break;
    }
}

void MapPanel::guardMapChange(Document& doc, const MapSession& session)
{
    const std::optional<int> loaded = session.loadedMap();
    if (!loaded || !session.dirty() || doc.play != PlayState::Stopped || doc.prefabEdit)
        return;
    const entt::entity terrain = firstTerrain(doc);
    if (terrain == entt::null || terrain != session.terrainEntity())
        return;
    int& map = doc.scene.registry().get<kast::Terrain>(terrain).map;
    if (map == *loaded)
        return;
    pending_ = {Pending::Kind::PickMap, map}; // applied by the prompt's Save / Don't Save (run), dropped by Cancel
    map = *loaded;
    askUnsaved_ = true;
}

bool MapPanel::sceneInput(MapSession& session, const SceneViewport::Input& input)
{
    hover_.reset();
    zoneHover_.reset();
    mapedit::MapDoc* z = session.doc();
    const ImGuiIO& io = ImGui::GetIO();
    const bool click = input.hovered && !input.cameraDrag && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool dragging = drag_.kind != Drag::Kind::None;
    const bool zones = zonePainting();
    if (!z || (tool_ == Tool::None && !zones && !click && !dragging))
        return false;
    std::string err;
    const auto info = z->info(err);
    if (!info)
        return tool_ != Tool::None;
    if (input.hovered || stroking_ || dragging || rect_ || zoneLast_) { // the ground under the mouse (in-memory heights)
        const kast::Ray ray = kast::screenRay(input.viewProj, io.MousePos.x - input.x, io.MousePos.y - input.y,
                                              input.width, input.height, bgfx::getCaps()->homogeneousDepth);
        hover_ = mapedit::raycast(*info, z->state, {ray.origin.x, ray.origin.y, ray.origin.z},
                               {ray.direction.x, ray.direction.y, ray.direction.z}, kPickDistance);
    }
    if (click && hover_) // whatever the tool: the Terrain section's heightmap tile
        tile_ = km::tile_of((*hover_)[0], (*hover_)[2]);
    if (zones)
        return zoneInput(*z, *info, click);
    switch (tool_) {
    case Tool::Raise:
    case Tool::Lower:
    case Tool::Flatten:
        sculptInput(*z, *info, click);
        break;
    case Tool::SetSpawn:
        if (click && hover_) {
            z->edit([&](mapedit::MapState& s) { s.manifest["spawn"]["pos"] = mapedit::vec3_json(*hover_); });
            kast::log::info(std::format("Map: player spawn at {:.1f}, {:.1f}, {:.1f}", (*hover_)[0], (*hover_)[1],
                                        (*hover_)[2]));
        }
        break;
    default: // areas, spawns, paths, objects, Scatter; handle drags with no tool
        return objectsInput(session, *z, *info, {nullptr, input.viewProj, input.x, input.y, input.width, input.height, *info,
                                              z->state},
                            click);
    }
    return true;
}

void MapPanel::drawSceneOverlay(const MapSession& session, ImDrawList* draw, const float* viewProj, float x, float y,
                                 float width, float height, std::array<float, 2> centre) const
{
    const mapedit::MapDoc* z = session.doc();
    std::string err;
    const auto info = z ? z->info(err) : std::nullopt;
    if (!info)
        return;
    draw->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);
    const GroundDraw g{draw, viewProj, x, y, width, height, *info, z->state, centre};
    if (zoneOverlay_)
        drawZoneOverlay(g, *z, *info);
    drawObjectOverlays(g);
    if (hover_ && (tool_ == Tool::Raise || tool_ == Tool::Lower || tool_ == Tool::Flatten))
        g.circle((*hover_)[0], (*hover_)[2], brush_.radius, kBrushColor);
    draw->PopClipRect();
}

void MapPanel::drawTopBar(Document& doc, MapSession& session)
{
    mapedit::MapDoc* z = session.doc();
    if (z) {
        ImGui::Text("maps/%s (id %d)", z->folder.c_str(), z->state.manifest.value("id", 0));
        if (z->dirty()) {
            ImGui::SameLine();
            ImGui::TextColored(kDirtyColor, "* unsaved");
        }
    } else {
        ImGui::TextDisabled("No map open");
    }
    ImGui::BeginDisabled(!z);
    if (ImGui::Button("Save Map"))
        save(doc, session);
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
    // The sub-tab's Scene view tools, wrapped to the tab's width.
    if (tab_ == Tab::Terrain) {
        toolButton("Raise", Tool::Raise);
        toolButton("Lower", Tool::Lower);
        toolButton("Flatten", Tool::Flatten);
        ImGui::NewLine();
    } else if (tab_ == Tab::Objects) {
        toolButton("Area Rect", Tool::AreaRect);
        toolButton("Area Polygon", Tool::AreaPolygon);
        toolButton("Place Spawn", Tool::PlaceSpawn);
        toolButton("Path Points", Tool::PathPoints);
        toolButton("Place Object", Tool::PlaceObject);
        toolButton("Scatter", Tool::Scatter);
        ImGui::NewLine();
    }
    ImGui::EndDisabled();
    if (tool_ != Tool::None)
        ImGui::TextDisabled("Left mouse in the Scene view: the tool. Esc: no tool.");
    ImGui::Separator();
}

void MapPanel::drawTerrainEntity(Document& doc, const MapSession& session)
{
    const auto terrains = doc.scene.registry().view<kast::Terrain>();
    if (terrains.empty()) {
        ImGui::TextWrapped("This scene has no Terrain entity. The map is the map a Terrain entity names: add one, or "
                           "pick or create a map below (that adds one too).");
        if (ImGui::Button("Add Terrain entity"))
            setTerrainMap(doc, session.project().maps.empty() ? 0 : session.project().maps.front().info.id);
        return;
    }
    if (terrains.size() > 1) {
        const kast::Name* name = doc.scene.registry().try_get<kast::Name>(terrains.front());
        ImGui::TextWrapped("%zu Terrain entities: the map is the map of \"%s\" (the one Play streams too).",
                           terrains.size(), name ? name->value.c_str() : "");
    }
    if (!session.error().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("The Terrain map did not load: %s", session.error().c_str());
        ImGui::PopStyleColor();
    }
}

void MapPanel::drawMap(Document& doc, MapSession& session)
{
    if (!header("Map", "Map", true))
        return;
    const mapedit::MapDoc* z = session.doc();
    const std::string preview = z ? z->folder : "(none)";
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("New Map...").x -
                            style.FramePadding.x * 2.0f - style.ItemSpacing.x);
    if (ImGui::BeginCombo("##map", preview.c_str())) {
        for (const mapedit::ProjectMap& m : session.project().maps) {
            const std::string label = std::format("{}  (id {}, {})", m.folder, m.info.id, m.info.name);
            const bool open = z && z->folder == m.folder;
            if (ImGui::Selectable(label.c_str(), open) && !open)
                request(doc, session, {Pending::Kind::PickMap, m.info.id});
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Map..."))
        request(doc, session, {Pending::Kind::NewMap, 0});
    if (z && std::ranges::none_of(session.project().maps, [&](const mapedit::ProjectMap& m) { return m.folder == z->folder; }))
        ImGui::TextDisabled("maps/%s is new: Save Map writes it", z->folder.c_str());
    if (mapedit::MapDoc* open = session.doc())
        drawManifest(*open, session);
}

void MapPanel::drawManifest(mapedit::MapDoc& z, const MapSession& session)
{
    mapedit::Json& m = z.state.manifest;
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
            z.edit([](mapedit::MapState& s) { s.manifest.erase("material"); });
        for (const std::string& path : session.materials())
            if (ImGui::Selectable(path.c_str(), path == material))
                z.edit([&](mapedit::MapState& s) { s.manifest["material"] = path; });
        ImGui::EndCombo();
    }

    // World (one context for everyone) or Instance (a copy per instance key): docs/services/world/instances.html.
    const bool instance = m.value("type", std::string("world")) == "instance";
    int type = instance ? 1 : 0;
    constexpr const char* kTypes[] = {"World", "Instance"};
    if (ImGui::Combo("Type", &type, kTypes, 2) && (type == 1) != instance)
        z.edit([&](mapedit::MapState& s) { mapedit::set_instance(s, type == 1, defaultExit(session, z.folder)); });
    if (instance)
        drawInstance(z, session);

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
        const float y = mapedit::ground_y(*info, z.state, xz[0], xz[1]).value_or(info->spawn[1]);
        m["spawn"]["pos"] = mapedit::vec3_json({xz[0], y, xz[1]});
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
        z.edit([&](mapedit::MapState& s) {
            if (nav)
                s.manifest["nav"] = mapedit::Json::object();
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
}

void MapPanel::drawInstance(mapedit::MapDoc& z, const MapSession& session)
{
    mapedit::Json& m = z.state.manifest;
    ImGui::SeparatorText("Instance");
    bool unlimited = !m.contains("maxPlayers");
    if (ImGui::Checkbox("Unlimited players", &unlimited))
        z.edit([&](mapedit::MapState& s) { mapedit::set_max_players(s, unlimited ? std::nullopt : std::optional<uint32_t>(5)); });
    if (!unlimited) {
        int players = m["maxPlayers"].is_number_integer() ? m["maxPlayers"].get<int>() : 1;
        if (ImGui::InputInt("Max players", &players))
            m["maxPlayers"] = unsigned(std::max(players, 1));
        undoScope(z);
    }
    int timeout = m.contains("emptyTimeoutSec") && m["emptyTimeoutSec"].is_number_integer() ? m["emptyTimeoutSec"].get<int>() : 300;
    if (ImGui::InputInt("Empty timeout (s)", &timeout))
        m["emptyTimeoutSec"] = unsigned(std::clamp(timeout, 0, 31536000));
    undoScope(z);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An empty copy is destroyed this long after its last player left (0: at once)");

    // Where a character goes when its copy is gone (a relog after the timeout or a restart): a world map's point.
    ImGui::SeparatorText("Exit");
    const mapedit::Json exit = m.value("exit", mapedit::Json::object());
    km::Portal e;
    e.map = exit.value("map", mapedit::Json()).is_number_unsigned() ? exit["map"].get<uint16_t>() : 0;
    if (const mapedit::Json& p = exit.value("pos", mapedit::Json()); p.is_array() && p.size() == 3)
        for (size_t i = 0; i < 3; ++i)
            e.pos[i] = p[i].is_number() ? p[i].get<float>() : 0.0f;
    e.heading = exit.value("heading", mapedit::Json()).is_number() ? exit["heading"].get<float>() : 0.0f;
    const mapedit::ProjectMap* to = nullptr;
    for (const mapedit::ProjectMap& pm : session.project().maps)
        if (pm.info.id == e.map && pm.folder != z.folder)
            to = &pm;
    const std::string preview = to ? std::format("{}  (id {})", to->folder, e.map) : std::format("map {}", e.map);
    if (ImGui::BeginCombo("Exit map", preview.c_str())) {
        for (const mapedit::ProjectMap& pm : session.project().maps) {
            if (pm.folder == z.folder || pm.info.instanced())
                continue; // an exit is on a world map
            const std::string label = std::format("{}  (id {}, {})", pm.folder, pm.info.id, pm.info.name);
            if (ImGui::Selectable(label.c_str(), pm.info.id == e.map) && pm.info.id != e.map)
                z.edit([&](mapedit::MapState& s) { mapedit::set_exit(s, {pm.info.id, pm.info.spawn, pm.info.spawnHeading}); });
        }
        ImGui::EndCombo();
    }
    if (ImGui::InputFloat3("Exit x, y, z", e.pos.data(), "%.1f"))
        mapedit::set_exit(z.state, e);
    undoScope(z);
    float heading = e.heading * 180.0f / bx::kPi;
    if (ImGui::SliderFloat("Exit heading", &heading, -180.0f, 180.0f, "%.0f deg")) {
        e.heading = heading * bx::kPi / 180.0f;
        mapedit::set_exit(z.state, e);
    }
    undoScope(z);
    std::string err;
    if (const auto info = z.info(err))
        if (const auto problem = km::exit_error(*info, to ? &to->info : nullptr))
            ImGui::TextColored(kErrorColor, "map.json: %s", problem->c_str());
}

void MapPanel::drawTiles(Document& doc, MapSession& session, FileActions& files, bool locked)
{
    if (header("Tiles", "Tiles", true))
        drawTileGrid(doc, session, files, locked);
}

void MapPanel::drawTileGrid(Document& doc, MapSession& session, FileActions& files, bool locked)
{
    mapedit::MapDoc* z = session.doc();
    std::string err;
    const std::optional<km::MapInfo> info = z ? z->info(err) : std::nullopt;
    if (!info || info->tiles.empty() || session.terrainEntity() == entt::null) {
        ImGui::TextDisabled("No map open");
        return;
    }
    // Zoom, pan and hover stay live while read-only; clicks are ignored then.
    ImGui::EndDisabled();
    const std::optional<km::TileKey> t = tiles_.draw(doc, session, files, *info, locked, zone_);
    ImGui::BeginDisabled(locked);
    if (!t)
        return;
    if (tiles_.mode() == MapTiles::Mode::Zones) {
        paintZone(*z, *info, *t);
        return;
    }
    if (!info->has_tile(*t)) {
        if (mapedit::within_cap(*info, *t))
            z->edit([&](mapedit::MapState& s) { mapedit::add_tile(*info, s, *t); });
    } else if (mapedit::declared_tiles(*info).size() > 1) {
        removeTile_ = *t;
    }
}

void MapPanel::drawProblems(const MapSession& session)
{
    // New problems (a load or a blocked save): expand the section.
    if (session.problems().size() != shownProblems_)
        openProblems_ = openProblems_ || !session.problems().empty();
    shownProblems_ = session.problems().size();
    if (std::exchange(openProblems_, false))
        ImGui::SetNextItemOpen(true);
    // Above the sub-tabs, always: its count in the error color while there are any.
    const std::string label = std::format("Problems ({})###Problems", session.problems().size());
    if (!session.problems().empty())
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    const bool open = header(label.c_str(), "Problems");
    if (!session.problems().empty())
        ImGui::PopStyleColor();
    if (!open)
        return;
    if (!session.doc())
        ImGui::TextDisabled("No map open");
    else if (session.problems().empty())
        ImGui::TextColored(kOkColor, "No problems");
    for (const mapedit::MapError& e : session.problems()) {
        ImGui::TextColored(kErrorColor, "%s:", e.file.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", e.message.c_str());
    }
}

void MapPanel::toolButton(const char* label, Tool tool)
{
    const bool active = tool_ == tool;
    const ImGuiStyle& style = ImGui::GetStyle();
    if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f)
        ImGui::NewLine();
    // Idle tools grey, the active one in the highlight with a border: told apart at a glance.
    ImGui::PushStyleColor(ImGuiCol_Button, active ? kToolActive : kToolIdle);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 1.0f)); // #000000
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 2.0f : 0.0f);
    if (ImGui::Button(label))
        tool_ = active ? Tool::None : tool;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
}

void MapPanel::request(Document& doc, MapSession& session, Pending pending)
{
    if (session.dirty()) {
        pending_ = pending;
        askUnsaved_ = true;
        return;
    }
    run(doc, session, pending);
}

void MapPanel::run(Document& doc, MapSession& session, const Pending& pending)
{
    tool_ = Tool::None;
    switch (pending.kind) {
    case Pending::Kind::PickMap:
        setTerrainMap(doc, pending.map); // the session loads it next frame
        break;
    case Pending::Kind::NewMap:
        newMap_ = {.id = mapedit::next_map_id(session.project())};
        openNewMap_ = true;
        askReplace_ = false;
        break;
    case Pending::Kind::None: break;
    }
}

void MapPanel::setTerrainMap(Document& doc, int id)
{
    entt::entity terrain = firstTerrain(doc);
    const auto idOf = [&](entt::entity entity) { return doc.scene.registry().get<kast::PersistentId>(entity).value; };
    beginSceneEdit(doc, "Set Terrain Map",
                   {.entities = terrain == entt::null ? std::vector<kast::EntityId>() : std::vector{idOf(terrain)}});
    if (terrain == entt::null) {
        terrain = doc.scene.create("Terrain");
        doc.scene.registry().emplace<kast::Terrain>(terrain);
        doc.selected = terrain;
        kast::log::info("Map: added a Terrain entity");
    }
    doc.scene.registry().get<kast::Terrain>(terrain).map = id;
    markDirty(doc);
    endSceneEdit(doc, {idOf(terrain)});
}

void MapPanel::drawUnsavedPrompt(Document& doc, MapSession& session)
{
    if (std::exchange(askUnsaved_, false))
        ImGui::OpenPopup(kUnsavedPopup);
    if (!ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("maps/%s has unsaved changes. Save them before changing the map?",
                session.doc() ? session.doc()->folder.c_str() : "");
    if (ImGui::Button("Save")) {
        ImGui::CloseCurrentPopup();
        if (save(doc, session)) // blocked: the problems show, the edits stay
            run(doc, session, std::exchange(pending_, {}));
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save")) {
        ImGui::CloseCurrentPopup();
        session.reload(); // the edits go even if the New Map dialog is cancelled
        run(doc, session, std::exchange(pending_, {}));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        pending_ = {};
    }
    ImGui::EndPopup();
}

bool MapPanel::openWizard(Document& doc, MapSession& session, std::string_view spec)
{
    std::vector<std::string_view> parts;
    for (const auto part : std::views::split(spec, ','))
        parts.emplace_back(part.begin(), part.end());
    NewMap filled;
    filled.name = std::string(parts[0]);
    if (parts.size() != 1 &&
        (parts.size() != 6 || !parseNumber(parts[1], filled.width) || !parseNumber(parts[2], filled.depth) ||
         !parseNumber(parts[3], filled.min) || !parseNumber(parts[4], filled.max) ||
         (parts[5] != "flat" && parts[5] != "generated")))
        return false;
    filled.generated = parts.size() == 6 && parts[5] == "generated";
    request(doc, session, {Pending::Kind::NewMap, 0});
    if (openNewMap_) { // no unsaved-map prompt in between
        filled.id = newMap_.id;
        newMap_ = filled;
    }
    return true;
}

std::string MapPanel::newMapBlocker(const MapSession& session) const
{
    const NewMap& w = newMap_;
    if (w.folder.empty())
        return "Enter a name";
    if (std::filesystem::exists(session.project().root / "maps" / fromUtf8(w.folder)))
        return std::format("maps/{} already exists: choose another name (or folder under Advanced)", w.folder);
    if (std::ranges::any_of(session.project().maps, [&](const mapedit::ProjectMap& m) { return m.info.id == w.id; }))
        return std::format("Id {} is already used", w.id);
    if (!(w.min < w.max))
        return "Height Max must be above Min";
    return {};
}

void MapPanel::drawNewMapDialog(Document& doc, MapSession& session, FileActions& files)
{
    if (std::exchange(openNewMap_, false))
        ImGui::OpenPopup(kNewMapPopup);
    if (!ImGui::BeginPopupModal(kNewMapPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pressCreate_ = pressReplace_ = false;
        return;
    }
    NewMap& w = newMap_;
    ImGui::InputText("Name", &w.name);
    if (!w.folderEdited)
        w.folder = folderFor(w.name);
    ImGui::InputInt("Tiles X", &w.width);
    ImGui::InputInt("Tiles Z", &w.depth);
    w.width = std::clamp(w.width, 1, mapedit::kMaxMapTiles);
    w.depth = std::clamp(w.depth, 1, mapedit::kMaxMapTiles);
    ImGui::InputFloat("Height Min", &w.min, 0.0f, 0.0f, "%.1f m");
    ImGui::InputFloat("Height Max", &w.max, 0.0f, 0.0f, "%.1f m");
    int start = w.generated ? 1 : 0;
    ImGui::TextUnformatted("Start terrain");
    ImGui::SameLine();
    ImGui::RadioButton("Flat", &start, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Generated", &start, 1);
    w.generated = start == 1;
    if (w.generated)
        ImGui::TextDisabled("Generate's settings (Terrain tab): seed %u, scale %.0f m", noise_.seed, noise_.scale);
    if (ImGui::TreeNode("Advanced")) {
        if (ImGui::InputText("Folder", &w.folder))
            w.folderEdited = true;
        ImGui::InputInt("Id", &w.id);
        w.id = std::clamp(w.id, 0, 0xFFFF);
        ImGui::TreePop();
    }
    ImGui::TextDisabled("maps/%s, id %d: one zone of every tile; saved with the scene", w.folder.c_str(), w.id);

    const std::string blocker = newMapBlocker(session);
    if (!blocker.empty() && !w.name.empty())
        ImGui::TextColored(kErrorColor, "%s", blocker.c_str());
    ImGui::BeginDisabled(!blocker.empty());
    bool create = ImGui::Button("Create");
    ImGui::EndDisabled();
    if (std::exchange(pressCreate_, false)) {
        create = blocker.empty();
        if (!create)
            kast::log::info("Map: New Map: Create is disabled: " + blocker);
    }
    if (create || askReplace_) {
        const bool confirmed = std::exchange(askReplace_, false);
        if (session.doc() && !confirmed) { // the scene's Terrain names a map: ask first
            ImGui::OpenPopup(kReplacePopup);
            kast::log::info(std::format("Map: New Map: the scene's Terrain uses maps/{}; asking before replacing the link",
                                        session.doc()->folder));
        } else if (createMap(doc, session, files)) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(kReplacePopup)))
        ImGui::CloseCurrentPopup();
    if (!w.error.empty())
        ImGui::TextColored(kErrorColor, "%s", w.error.c_str());

    if (ImGui::BeginPopupModal(kReplacePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string old = session.doc() ? session.doc()->folder : "";
        ImGui::Text("This scene's Terrain uses maps/%s.", old.c_str());
        ImGui::Text("Create points it at maps/%s instead. maps/%s stays on disk, unchanged.", w.folder.c_str(),
                    old.c_str());
        if (ImGui::Button("Replace") || std::exchange(pressReplace_, false)) {
            ImGui::CloseCurrentPopup();
            askReplace_ = true; // the wizard creates next frame, past the question
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndPopup();
}

bool MapPanel::createMap(Document& doc, MapSession& session, FileActions& files)
{
    NewMap& w = newMap_;
    w.error.clear();
    if (!session.newMap(w.folder, w.name.empty() ? w.folder : w.name, uint16_t(w.id), w.width, w.depth, w.error))
        return false;
    // The wizard's choices are the new map's first state, not undo steps: there is nothing before it to go back to.
    mapedit::MapDoc& z = *session.doc();
    std::string err;
    mapedit::zones_list(*z.info(err), z.state, z.folder); // one zone of every tile
    mapedit::set_height_range(*z.info(err), z.state, w.min, w.max);
    const km::MapInfo info = *z.info(err);
    if (w.generated)
        mapedit::generate_heights(info, z.state, noise_);
    z.state.manifest["spawn"]["pos"][1] = kast::jsonNumber(*mapedit::ground_y(info, z.state, info.spawn[0], info.spawn[2]));
    setTerrainMap(doc, w.id);
    map_cli::frameMap(doc.camera, z); // in view straight away, as Focus on the map
    kast::log::info(std::format("Map: New Map maps/{}: {} x {} tiles, heights {} .. {} m, {} terrain, one zone", w.folder,
                                w.width, w.depth, w.min, w.max, w.generated ? "generated" : "flat"));
    files.save(); // the scene (asking where if untitled), then the map (FileActions::extra)
    return true;
}

void MapPanel::drawRemoveTilePrompt(MapSession& session)
{
    if (removeTile_ && !ImGui::IsPopupOpen(kRemoveTilePopup))
        ImGui::OpenPopup(kRemoveTilePopup);
    if (!ImGui::BeginPopupModal(kRemoveTilePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    mapedit::MapDoc* z = session.doc();
    const km::TileKey t = removeTile_.value_or(km::TileKey{});
    if (!z) { // the map went away (a scene change) while asking
        removeTile_.reset();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("Remove tile %s from maps/%s?", mapedit::tile_name(t).c_str(), z->folder.c_str());
    const std::vector<std::string> files = mapedit::tile_files(session.project(), z->folder, t);
    if (files.empty())
        ImGui::TextDisabled("It has no files on disk.");
    else
        ImGui::Text("These files are deleted (to the Recycle Bin) when you save:");
    for (const std::string& f : files)
        ImGui::BulletText("%s", f.c_str());
    if (const auto it = z->state.spawns.find(t);
        it != z->state.spawns.end() && !it->second.value("spawns", mapedit::Json::array()).empty()) {
        ImGui::Text("Its spawns are lost:");
        for (const mapedit::Json& s : it->second["spawns"])
            ImGui::BulletText("%s (%s)", s.value("id", "").c_str(), s.value("npc", "").c_str());
    }
    if (ImGui::Button("Remove")) {
        std::string err;
        if (const auto info = z->info(err))
            z->edit([&](mapedit::MapState& s) { mapedit::remove_tile(*info, s, t); });
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
