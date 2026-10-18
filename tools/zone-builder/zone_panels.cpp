#include "zone_panels.h"

#include <editor_settings.h>
#include <scene_io.h> // jsonNumber
#include <ui_common.h>

#include <app.h>
#include <files.h>
#include <log.h>

#include <SDL3/SDL.h>
#include <bx/math.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <random>

namespace zone {
namespace {

namespace fs = std::filesystem;
namespace km = kast::map;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The path picked in an async file dialog and which dialog it answers (userdata); SDL may call back on another thread.
std::mutex pickedMutex;
std::optional<std::pair<int, fs::path>> picked;

void SDLCALL onPicked(void* dialog, const char* const* files, int) {
    if (!files || !files[0]) return; // cancelled or failed
    std::lock_guard lock(pickedMutex);
    picked = {int(reinterpret_cast<intptr_t>(dialog)), fs::path(std::u8string(reinterpret_cast<const char8_t*>(files[0])))};
}

void* dialogData(int dialog) { return reinterpret_cast<void*>(intptr_t(dialog)); }

constexpr SDL_DialogFileFilter kPngFilter{"16-bit grayscale PNG", "png"};
constexpr ImU32 kBrushColor = IM_COL32(255, 204, 0, 255);

// One undo step per edit of the last item: from its activation to its deactivation.
void undoScope(ZoneDoc& doc) {
    if (ImGui::IsItemActivated()) doc.begin();
    if (ImGui::IsItemDeactivated()) doc.commit();
}

} // namespace

void Session::print(std::string line) {
    kast::log::info(line);
    log.push_back(std::move(line));
}

bool Session::openProject(const fs::path& root) {
    if (!fs::is_directory(root / "maps") && !fs::exists(root / "project.kast")) {
        print(std::format("Open project: {} has no maps/ folder or project.kast", utf8(root)));
        return false;
    }
    project = scan_project(fs::absolute(root).lexically_normal());
    doc.reset();
    problems = project->errors;
    scanMaterials();
    npcs = npc_names(project->root);
    print(std::format("Opened project {} ({} maps)", utf8(project->root), project->maps.size()));
    return true;
}

void Session::scanMaterials() {
    materials.clear();
    for (const fs::path& file : kast::listFiles(project->root))
        if (file.extension() == ".mat") {
            const std::u8string rel = file.lexically_relative(project->root).generic_u8string();
            materials.emplace_back(rel.begin(), rel.end());
        }
}

bool Session::openMap(const std::string& folder) {
    if (!project) return false;
    std::string err;
    auto loaded = load_map(*project, folder, err);
    if (!loaded) {
        problems = {{"maps/" + folder, err}};
        print("Open map failed: " + err);
        return false;
    }
    doc = std::move(loaded);
    problems = validate(*project, *doc);
    print(std::format("Opened maps/{}{}", folder, problems.empty() ? "" : " (with problems)"));
    return true;
}

bool Session::newMap(const std::string& folder, const std::string& name, uint16_t id, int width, int depth) {
    if (!project) return false;
    std::string err;
    auto made = new_map(*project, folder, name, id, width, depth, err);
    if (!made) {
        print("New map: " + err);
        return false;
    }
    doc = std::move(made);
    problems.clear();
    print(std::format("New map maps/{} (id {}, {}x{} tiles); save to write it", folder, id, width, depth));
    return true;
}

bool Session::save() {
    if (!project || !doc) return false;
    problems = save_map(*project, *doc, recycle);
    if (!problems.empty()) {
        print(std::format("Save blocked: {} problem(s), nothing written", problems.size()));
        for (const ZoneError& e : problems) kast::log::error(e.file + ": " + e.message);
        return false;
    }
    print("Saved maps/" + doc->folder);
    scanMaterials(); // a generated terrain material may be new
    return true;
}

bool Session::generateHeights(const NoiseParams& params) {
    std::string err;
    const auto info = doc ? doc->info(err) : std::nullopt;
    if (!info) return false;
    doc->edit([&](ZoneState& s) {
        generate_heights(*info, s, params);
        // The spawn stays on the ground (it is on the map: the manifest parsed).
        s.manifest["spawn"]["pos"][1] = kast::jsonNumber(*ground_y(*info, s, info->spawn[0], info->spawn[2]));
    });
    print(std::format("Generated heights (seed {}, scale {} m, {} octaves)", params.seed, params.scale, params.octaves));
    return true;
}

bool Session::setTerrainColors(const Gradient& gradient) {
    if (!doc || gradient.size() < 2) return false;
    doc->edit([&](ZoneState& s) { s.manifest["terrainColors"] = gradient_json(gradient); });
    return true;
}

bool Session::createTerrainMaterial() {
    if (!doc) return false;
    std::string err;
    const bool hasColors = parse_gradient(doc->state.manifest, err).has_value();
    doc->edit([&](ZoneState& s) {
        if (!hasColors) s.manifest["terrainColors"] = gradient_json(default_gradient());
        s.manifest["terrainUv"] = "map";
        s.manifest["material"] = terrain_material_path(doc->folder);
    });
    print("Terrain material " + terrain_material_path(doc->folder) + " and " + terrain_texture_path(doc->folder) +
          " assigned; written on save");
    return true;
}

bool Session::sculpt(const Brush& brush, float x, float z, float amount) {
    std::string err;
    const auto info = doc ? doc->info(err) : std::nullopt;
    if (!info) return false;
    TileSet touched;
    doc->edit([&](ZoneState& s) { touched = apply_brush(*info, s, x, z, brush, amount); });
    return !touched.empty();
}

bool Session::setHeightRange(float min, float max) {
    std::string err;
    const auto info = doc ? doc->info(err) : std::nullopt;
    if (!info || !(min <= max)) {
        print("Height range: min must be <= max");
        return false;
    }
    size_t clipped = 0;
    doc->edit([&](ZoneState& s) { clipped = set_height_range(*info, s, min, max); });
    print(std::format("Height range now {} .. {} m{}", min, max, clipped ? std::format(" ({} samples clipped)", clipped) : ""));
    return true;
}

bool Session::importPng(TileKey tile, const fs::path& file) {
    std::string err, bytes;
    const auto info = doc ? doc->info(err) : std::nullopt;
    if (!info || !info->has_tile(tile)) {
        print("Import PNG: tile " + tile_name(tile) + " is not on the map");
        return false;
    }
    bool ok = kast::readFile(file, bytes);
    if (!ok) err = "cannot read the file";
    else doc->edit([&](ZoneState& s) { ok = import_png16(*info, s, tile, bytes, err); });
    print(ok ? std::format("Imported {} into tile {}", utf8(file), tile_name(tile))
             : std::format("Import PNG {}: {}", utf8(file), err));
    return ok;
}

bool Session::exportPng(TileKey tile, const fs::path& file) {
    std::string err;
    const auto info = doc ? doc->info(err) : std::nullopt;
    if (!info || !info->has_tile(tile)) {
        print("Export PNG: tile " + tile_name(tile) + " is not on the map");
        return false;
    }
    const bool ok = kast::writeFileAtomic(file, encode_png16(tile_heights(doc->state, tile)));
    print(ok ? std::format("Exported tile {} to {}", tile_name(tile), utf8(file)) : "Export PNG: cannot write " + utf8(file));
    return ok;
}

bool Session::startBake() {
    if (!project || !doc) return false;
    if (doc->dirty()) {
        print("Bake: save the map first");
        return false;
    }
    problems = validate(*project, *doc);
    if (!problems.empty()) {
        print(std::format("Bake: {} problem(s), fix them first", problems.size()));
        return false;
    }
    const fs::path exe = find_mapbake(mapbakeSetting);
    std::string err;
    if (exe.empty()) err = "kast_mapbake.exe not found; set its path in the Bake panel";
    const fs::path dir = project->root / "maps" / fs::path(std::u8string(doc->folder.begin(), doc->folder.end()));
    if (exe.empty() || !bake.start(exe, {dir}, err)) {
        print("Bake: " + err);
        return false;
    }
    bakeLog = {"> " + utf8(exe) + " " + utf8(dir)};
    bakeExit.reset();
    print("Baking maps/" + doc->folder + "...");
    return true;
}

void Session::pollBake() {
    for (std::string& line : bake.takeLines()) bakeLog.push_back(std::move(line));
    if (const auto code = bake.exitCode(); code && code != bakeExit && !bake.running()) {
        bakeExit = code;
        bakeLog.push_back(std::format("exit code {}", *code));
        print(std::format("Bake finished: exit code {}{}", *code, *code == 0 ? "" : " (see the Bake panel)"));
    }
}

void Session::loadSettings() {
    if (!settingsFile.empty()) mapbakeSetting = editorSetting(loadEditorSettings(settingsFile), "mapbake");
}

void Session::saveSettings() {
    std::string err;
    if (!settingsFile.empty() && !saveEditorSetting(settingsFile, "mapbake", mapbakeSetting, err)) print("Settings: " + err);
}

void Panels::request(Session& session, Pending pending) {
    if (session.doc && session.doc->dirty()) {
        pending_ = std::move(pending);
        askUnsaved_ = true;
        return;
    }
    run(session, pending);
}

void Panels::run(Session& session, const Pending& pending) {
    switch (pending.kind) {
    case Pending::Kind::OpenProject:
        session.openProject(fs::path(std::u8string(pending.arg.begin(), pending.arg.end())));
        break;
    case Pending::Kind::OpenMap:
        rangeEdited_ = false;
        area_.reset();
        spawn_.clear();
        tool_ = Tool::None;
        if (session.openMap(pending.arg)) view_.frameMap(*session.doc);
        break;
    case Pending::Kind::NewMap:
        newFolder_.clear();
        newName_.clear();
        newId_ = session.project ? next_map_id(*session.project) : 0;
        openNewMap_ = true;
        break;
    case Pending::Kind::Quit:
        quitConfirmed_ = true;
        kast::requestQuit();
        break;
    case Pending::Kind::None:
        break;
    }
}

bool Panels::onQuitRequested(Session& session) {
    if (quitConfirmed_ || !session.doc || !session.doc->dirty()) return true;
    request(session, {Pending::Kind::Quit, {}});
    return false;
}

void Panels::pollDialogs(Session& session) {
    std::optional<std::pair<int, fs::path>> answer;
    {
        std::lock_guard lock(pickedMutex);
        answer.swap(picked);
    }
    if (!answer) return;
    switch (Dialog(answer->first)) {
    case Dialog::OpenProject: request(session, {Pending::Kind::OpenProject, utf8(answer->second)}); break;
    case Dialog::ImportPng: session.importPng(tile_, answer->second); break;
    case Dialog::ExportPng: {
        fs::path file = answer->second;
        if (file.extension() != ".png") file += ".png";
        session.exportPng(tile_, file);
        break;
    }
    }
}

void Panels::select(const Session& session, const std::string& id) {
    if (!session.doc) return;
    if (const auto area = find_area(session.doc->state, id)) area_ = area;
    if (find_spawn(session.doc->state, id)) spawn_ = id;
}

void Panels::draw(Session& session, SDL_Window* window) {
    // Docked tabs ignore focus while the dock is built in the first frames: keep asking for a while.
    constexpr int kShowPanelFrames = 10;
    if (!showPanel_.empty() && showPanelFrames_++ < kShowPanelFrames) ImGui::SetWindowFocus(showPanel_.c_str());
    pollDialogs(session);
    shortcuts(session);
    drawMenu(session, window);
    drawMaps(session);
    drawManifest(session);
    drawTerrain(session, window);
    drawAreas(session);
    drawSpawns(session);
    drawBake(session);
    session.pollBake();
    drawHeightRangePrompt(session);
    updateTool(session);
    drawProblems(session);
    drawLog(session);
    drawUnsavedPrompt(session);
    drawNewMapDialog(session);

    std::string title = "Kast Zone Builder";
    if (session.doc) title += " - maps/" + session.doc->folder + (session.doc->dirty() ? " *" : "");
    if (window && title != SDL_GetWindowTitle(window)) SDL_SetWindowTitle(window, title.c_str());
}

void Panels::shortcuts(Session& session) {
    if (ImGui::GetTopMostPopupModal() || !session.doc) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) session.save();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) session.doc->undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal)) session.doc->redo();
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) view_.frameMap(*session.doc);
}

void Panels::drawMenu(Session& session, SDL_Window* window) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Project..."))
            SDL_ShowOpenFolderDialog(onPicked, dialogData(int(Dialog::OpenProject)), window, nullptr, false);
        ImGui::BeginDisabled(!session.project);
        if (ImGui::MenuItem("New Map...")) request(session, {Pending::Kind::NewMap, {}});
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!session.doc);
        if (ImGui::MenuItem("Save", "Ctrl+S")) session.save();
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) kast::requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, session.doc && session.doc->canUndo())) session.doc->undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, session.doc && session.doc->canRedo())) session.doc->redo();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame Map", "F", false, session.doc.has_value())) view_.frameMap(*session.doc);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Panels::drawMaps(Session& session) {
    if (ImGui::Begin("Maps")) {
        if (!session.project) {
            ImGui::TextWrapped("No project open. File > Open Project...");
        } else {
            ImGui::TextDisabled("%s", utf8(session.project->root).c_str());
            if (ImGui::Button("New Map...")) request(session, {Pending::Kind::NewMap, {}});
            ImGui::Separator();
            for (const ProjectMap& m : session.project->maps) {
                const bool open = session.doc && session.doc->folder == m.folder;
                const std::string label = std::format("{}  (id {}, {})", m.folder, m.info.id, m.info.name);
                if (ImGui::Selectable(label.c_str(), open) && !open) request(session, {Pending::Kind::OpenMap, m.folder});
            }
            if (session.doc && std::none_of(session.project->maps.begin(), session.project->maps.end(),
                                             [&](const ProjectMap& m) { return m.folder == session.doc->folder; }))
                ImGui::Selectable((session.doc->folder + "  (new, unsaved)").c_str(), true);
        }
    }
    ImGui::End();
}

void Panels::drawManifest(Session& session) {
    if (ImGui::Begin("Map") && session.doc) {
        ZoneDoc& doc = *session.doc;
        Json& m = doc.state.manifest;
        ImGui::Text("maps/%s", doc.folder.c_str());

        int id = m.value("id", 0);
        if (ImGui::InputInt("Id", &id)) m["id"] = unsigned(std::clamp(id, 0, 0xFFFF)); // the parser wants an unsigned number
        undoScope(doc);
        std::string name = m.value("name", std::string());
        if (ImGui::InputText("Name", &name)) m["name"] = name;
        undoScope(doc);

        const std::string material = m.value("material", std::string());
        if (ImGui::BeginCombo("Material", material.empty() ? "(default)" : material.c_str())) {
            if (ImGui::Selectable("(default)", material.empty())) doc.edit([](ZoneState& s) { s.manifest.erase("material"); });
            for (const std::string& path : session.materials)
                if (ImGui::Selectable(path.c_str(), path == material))
                    doc.edit([&](ZoneState& s) { s.manifest["material"] = path; });
            ImGui::EndCombo();
        }

        std::string err;
        const auto info = doc.info(err);
        if (!info) {
            ImGui::TextColored(kErrorColor, "map.json: %s", err.c_str());
            ImGui::End();
            return;
        }
        const auto range = km::height_range(*info);
        ImGui::Text("Height range: %.2f .. %.2f m (Terrain panel)", range[0], range[1]);

        ImGui::SeparatorText("Player spawn");
        float xz[2] = {info->spawn[0], info->spawn[2]};
        if (ImGui::InputFloat2("Position x, z", xz, "%.1f")) {
            const float y = ground_y(*info, doc.state, xz[0], xz[1]).value_or(info->spawn[1]);
            m["spawn"]["pos"] = vec3_json({xz[0], y, xz[1]});
        }
        undoScope(doc);
        float heading = info->spawnHeading * 180.0f / bx::kPi;
        if (ImGui::SliderFloat("Heading", &heading, -180.0f, 180.0f, "%.0f deg"))
            m["spawn"]["heading"] = kast::jsonNumber(heading * bx::kPi / 180.0f);
        undoScope(doc);
        ImGui::Text("y %.2f", info->spawn[1]);
        ImGui::SameLine();
        toolButton("Place on terrain", Tool::SetSpawn);
        ImGui::NewLine();

        ImGui::SeparatorText("Navmesh (kast_mapbake)");
        bool nav = m.contains("nav");
        if (ImGui::Checkbox("Own nav settings", &nav))
            doc.edit([&](ZoneState& s) {
                if (nav) s.manifest["nav"] = Json::object();
                else s.manifest.erase("nav");
            });
        if (nav && info->nav) {
            const km::NavParams p = *info->nav;
            const std::pair<const char*, float> fields[] = {{"agentRadius", p.agentRadius}, {"agentHeight", p.agentHeight},
                                                           {"maxClimb", p.maxClimb}, {"maxSlopeDeg", p.maxSlopeDeg},
                                                           {"cellSize", p.cellSize}};
            for (auto [key, value] : fields) {
                if (ImGui::InputFloat(key, &value, 0.0f, 0.0f, "%.2f")) m["nav"][key] = kast::jsonNumber(std::max(0.0f, value));
                undoScope(doc);
            }
        }

        ImGui::SeparatorText("Tiles");
        drawTileGrid(session, *info);
    }
    ImGui::End();
    drawRemoveTilePrompt(session);
}

void Panels::drawTileGrid(Session& session, const km::MapInfo& info) {
    ZoneDoc& doc = *session.doc;
    const TileSet tiles = declared_tiles(info);
    int32_t x0 = INT32_MAX, z0 = INT32_MAX, x1 = INT32_MIN, z1 = INT32_MIN;
    for (TileKey t : tiles) {
        x0 = std::min(x0, t.x - 1);
        z0 = std::min(z0, t.z - 1);
        x1 = std::max(x1, t.x + 1);
        z1 = std::max(z1, t.z + 1);
    }
    ImGui::TextDisabled("Click a free cell to add a tile, a tile to remove it. +z is up.");
    const float cell = ImGui::GetFrameHeight() * 1.6f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));
    for (int32_t z = z1; z >= z0; --z) { // +z up, as seen from above with the default camera
        for (int32_t x = x0; x <= x1; ++x) {
            const TileKey t{x, z};
            const bool declared = tiles.contains(t);
            ImGui::PushID(int(x * 7919 + z));
            if (!declared) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            if (ImGui::Button(declared ? tile_name(t).c_str() : "+", ImVec2(cell, cell))) {
                if (!declared) doc.edit([&](ZoneState& s) { add_tile(info, s, t); });
                else if (tiles.size() > 1) removeTile_ = t;
            }
            if (!declared) ImGui::PopStyleColor();
            ImGui::PopID();
            if (x < x1) ImGui::SameLine();
        }
    }
    ImGui::PopStyleVar();
}

void Panels::drawRemoveTilePrompt(Session& session) {
    if (removeTile_ && !ImGui::IsPopupOpen("Remove tile?")) ImGui::OpenPopup("Remove tile?");
    if (!ImGui::BeginPopupModal("Remove tile?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const TileKey t = removeTile_.value_or(TileKey{});
    ZoneDoc& doc = *session.doc;
    ImGui::Text("Remove tile %s from maps/%s?", tile_name(t).c_str(), doc.folder.c_str());
    const auto files = session.project ? tile_files(*session.project, doc.folder, t) : std::vector<std::string>();
    if (files.empty()) ImGui::TextDisabled("It has no files on disk.");
    else ImGui::Text("These files are deleted (to the Recycle Bin) when you save:");
    for (const std::string& f : files) ImGui::BulletText("%s", f.c_str());
    if (const auto it = doc.state.spawns.find(t); it != doc.state.spawns.end() && !it->second.value("spawns", Json::array()).empty()) {
        ImGui::Text("Its spawns are lost:");
        for (const Json& s : it->second["spawns"]) ImGui::BulletText("%s (%s)", s.value("id", "").c_str(), s.value("npc", "").c_str());
    }
    if (ImGui::Button("Remove")) {
        std::string err;
        if (const auto info = doc.info(err)) doc.edit([&](ZoneState& s) { remove_tile(*info, s, t); });
        removeTile_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        removeTile_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Panels::drawBake(Session& session) {
    if (ImGui::Begin("Bake")) {
        ImGui::TextWrapped("Bakes .server/nav/<tile>.navtile for the world server with kast_mapbake, on the saved map.");
        if (ImGui::InputText("kast_mapbake.exe", &session.mapbakeSetting)) mapbakeEdited_ = true;
        if (mapbakeEdited_ && !ImGui::IsItemActive()) {
            session.saveSettings();
            mapbakeEdited_ = false;
        }
        const auto exe = find_mapbake(session.mapbakeSetting);
        if (exe.empty()) ImGui::TextColored(kErrorColor, "kast_mapbake.exe not found: build services/world, or set its path");
        else ImGui::TextDisabled("Runs %s", utf8(exe).c_str());
        const bool canBake = session.doc && !session.doc->dirty() && !session.bake.running() && !exe.empty();
        ImGui::BeginDisabled(!canBake);
        if (ImGui::Button("Bake Navmesh")) session.startBake();
        ImGui::EndDisabled();
        if (session.doc && session.doc->dirty()) disabledTooltip("Save the map first");
        if (session.bake.running()) {
            ImGui::SameLine();
            ImGui::Text("Baking... %c", "|/-\\"[int(ImGui::GetTime() * 8.0) & 3]);
        } else if (session.bakeExit) {
            ImGui::SameLine();
            ImGui::TextColored(*session.bakeExit == 0 ? kOkColor : kErrorColor, "Exit code %d", *session.bakeExit);
        }
        if (ImGui::BeginChild("##bakeLog", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            for (const std::string& line : session.bakeLog) ImGui::TextUnformatted(line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void Panels::drawProblems(Session& session) {
    // New problems (a blocked save): bring the panel to the front. A docked tab shows a frame after focus, and not at
    // all while the dock is still being built at startup, so keep asking until it really shows.
    if (session.problems.size() != shownProblems_) focusProblems_ = !session.problems.empty();
    shownProblems_ = session.problems.size();
    if (focusProblems_) ImGui::SetNextWindowFocus();
    const std::string title = std::format("Problems ({})###Problems", session.problems.size());
    const bool visible = ImGui::Begin(title.c_str());
    const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
    if (focusProblems_ && visible && ImGui::IsWindowFocused() && (!window->DockIsActive || window->DockTabIsVisible))
        focusProblems_ = false;
    if (visible) {
        if (session.problems.empty()) ImGui::TextColored(kOkColor, "No problems");
        for (const ZoneError& e : session.problems) {
            ImGui::TextColored(kErrorColor, "%s:", e.file.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", e.message.c_str());
        }
    }
    ImGui::End();
}

void Panels::drawLog(Session& session) {
    if (ImGui::Begin("Log")) {
        for (const std::string& line : session.log) ImGui::TextWrapped("%s", line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::End();
}

void Panels::drawUnsavedPrompt(Session& session) {
    if (askUnsaved_) {
        ImGui::OpenPopup("Unsaved changes");
        askUnsaved_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("maps/%s has unsaved changes.", session.doc ? session.doc->folder.c_str() : "");
    if (ImGui::Button("Save")) {
        ImGui::CloseCurrentPopup();
        if (session.save()) run(session, pending_); // blocked: the problems show, the edits stay
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save")) {
        ImGui::CloseCurrentPopup();
        session.doc.reset(); // so the next request does not ask again
        run(session, pending_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Panels::drawNewMapDialog(Session& session) {
    if (openNewMap_) {
        ImGui::OpenPopup("New Map");
        openNewMap_ = false;
    }
    if (!ImGui::BeginPopupModal("New Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::InputText("Folder", &newFolder_);
    ImGui::InputText("Name", &newName_);
    ImGui::InputInt("Id", &newId_);
    ImGui::InputInt("Tiles X", &newWidth_);
    ImGui::InputInt("Tiles Z", &newDepth_);
    newId_ = std::clamp(newId_, 0, 0xFFFF);
    newWidth_ = std::clamp(newWidth_, 1, 8);
    newDepth_ = std::clamp(newDepth_, 1, 8);
    const bool used = session.project && std::any_of(session.project->maps.begin(), session.project->maps.end(),
                                                     [&](const ProjectMap& m) { return m.info.id == newId_; });
    if (used) ImGui::TextColored(kErrorColor, "Id %d is already used", newId_);
    ImGui::BeginDisabled(used || newFolder_.empty());
    if (ImGui::Button("Create")) {
        if (session.newMap(newFolder_, newName_.empty() ? newFolder_ : newName_, uint16_t(newId_), newWidth_, newDepth_)) {
            view_.frameMap(*session.doc);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    if (!session.log.empty() && session.log.back().starts_with("New map: ")) ImGui::TextColored(kErrorColor, "%s", session.log.back().c_str());
    ImGui::EndPopup();
}

void Panels::toolButton(const char* label, Tool tool) {
    const bool active = tool_ == tool;
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label)) {
        tool_ = active ? Tool::None : tool;
        polygon_.clear();
        rect_.reset();
    }
    if (active) ImGui::PopStyleColor();
    ImGui::SameLine();
}

void Panels::drawTerrain(Session& session, SDL_Window* window) {
    if (ImGui::Begin("Terrain") && session.doc) {
        ZoneDoc& doc = *session.doc;
        std::string err;
        const auto info = doc.info(err);
        toolButton("Raise", Tool::Raise);
        toolButton("Lower", Tool::Lower);
        toolButton("Flatten", Tool::Flatten);
        ImGui::NewLine();
        ImGui::SliderFloat("Radius", &brush_.radius, 1.0f, 128.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Strength", &strength_, 0.1f, 50.0f, tool_ == Tool::Flatten ? "%.1f /s" : "%.1f m/s",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Falloff", &brush_.falloff, 0.0f, 1.0f, "%.2f");
        if (tool_ == Tool::Flatten) {
            ImGui::InputFloat("Flatten to", &brush_.target, 0.0f, 0.0f, "%.2f m");
            ImGui::TextDisabled("Shift+click the terrain to pick the height");
        }
        if (tool_ != Tool::None) ImGui::TextDisabled("Left drag paints; right drag orbits");

        if (info) {
            ImGui::SeparatorText("Height range");
            const auto range = kast::map::height_range(*info);
            if (!ImGui::IsAnyItemActive() && !askRange_ && !rangeEdited_) {
                rangeMin_ = range[0];
                rangeMax_ = range[1];
            }
            if (ImGui::InputFloat("Min", &rangeMin_, 0.0f, 0.0f, "%.2f m")) rangeEdited_ = true;
            if (ImGui::InputFloat("Max", &rangeMax_, 0.0f, 0.0f, "%.2f m")) rangeEdited_ = true;
            ImGui::BeginDisabled((rangeMin_ == range[0] && rangeMax_ == range[1]) || rangeMin_ > rangeMax_);
            if (ImGui::Button("Apply range")) {
                rangeClips_ = count_clipped(*info, doc.state, rangeMin_, rangeMax_);
                if (rangeClips_ == 0) {
                    session.setHeightRange(rangeMin_, rangeMax_);
                    rangeEdited_ = false;
                } else {
                    askRange_ = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert")) rangeEdited_ = false;
            if (range[1] <= range[0]) ImGui::TextColored(kErrorColor, "Set a range (max > min) to sculpt");

            ImGui::SeparatorText("Heightmap");
            if (!info->has_tile(tile_)) tile_ = {info->tiles[0].x0, info->tiles[0].z0};
            ImGui::Text("Tile %s (click the terrain to choose)", tile_name(tile_).c_str());
            if (ImGui::Button("Import PNG..."))
                SDL_ShowOpenFileDialog(onPicked, dialogData(int(Dialog::ImportPng)), window, &kPngFilter, 1, nullptr, false);
            ImGui::SameLine();
            if (ImGui::Button("Export PNG..."))
                SDL_ShowSaveFileDialog(onPicked, dialogData(int(Dialog::ExportPng)), window, &kPngFilter, 1, nullptr);

            drawGenerate(session, range[1] > range[0]);
            drawTerrainColors(session);
        }
    }
    ImGui::End();
}

void Panels::drawGenerate(Session& session, bool hasRange) {
    ImGui::SeparatorText("Generate");
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &noise_.seed);
    ImGui::SameLine();
    if (ImGui::Button("Randomize")) noise_.seed = std::random_device{}();
    ImGui::SliderFloat("Scale", &noise_.scale, 16.0f, 4096.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderInt("Octaves", &noise_.octaves, 1, 16);
    ImGui::SliderFloat("Persistence", &noise_.persistence, 0.05f, 1.0f, "%.2f");
    ImGui::SliderFloat("Lacunarity", &noise_.lacunarity, 1.0f, 4.0f, "%.2f");
    ImGui::BeginDisabled(!hasRange);
    if (ImGui::Button("Generate heights")) session.generateHeights(noise_);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Replaces every tile's heights, over the whole height range");
}

void Panels::drawTerrainColors(Session& session) {
    ZoneDoc& doc = *session.doc;
    ImGui::SeparatorText("Height colors");
    std::string err;
    // Edits go to the manifest as they happen and are one undo step from the first change until no widget is active
    // (a drag, a color picker).
    if (!colorsEditing_ && !ImGui::IsAnyItemActive()) gradient_ = parse_gradient(doc.state.manifest, err).value_or(default_gradient());
    bool changed = false;
    std::optional<size_t> remove;
    for (size_t i = 0; i < gradient_.size(); ++i) {
        ImGui::PushID(int(i));
        float pct = gradient_[i].pos * 100.0f;
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::DragFloat("##pos", &pct, 0.25f, 0.0f, 100.0f, "%.1f %%", ImGuiSliderFlags_AlwaysClamp)) {
            gradient_[i].pos = pct / 100.0f;
            changed = true;
        }
        ImGui::SameLine();
        changed |= ImGui::ColorEdit3("##color", gradient_[i].rgb.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::BeginDisabled(gradient_.size() <= 2);
        if (ImGui::Button("Remove")) remove = i;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (changed) {
        if (!colorsEditing_) doc.begin();
        colorsEditing_ = true;
        doc.state.manifest["terrainColors"] = gradient_json(gradient_);
    }
    if (colorsEditing_ && !ImGui::IsAnyItemActive()) {
        doc.commit();
        colorsEditing_ = false;
    }
    if (remove && !colorsEditing_) {
        gradient_.erase(gradient_.begin() + std::ptrdiff_t(*remove));
        session.setTerrainColors(gradient_);
    }
    if (ImGui::Button("Add stop") && !colorsEditing_) {
        // Halfway between the two highest stops, in the color already there: the look does not change until edited.
        std::vector<float> pos;
        for (const ColorStop& c : gradient_) pos.push_back(c.pos);
        std::sort(pos.begin(), pos.end());
        const float at = (pos[pos.size() - 2] + pos.back()) / 2.0f;
        gradient_.push_back({at, gradient_color(gradient_, at)});
        session.setTerrainColors(gradient_);
    }
    ImGui::TextDisabled("Position: %% of the height range (0 = min, 100 = max)");

    ImGui::SeparatorText("Terrain texture");
    const std::string material = terrain_material_path(doc.folder);
    if (ImGui::Button("Create texture & material")) session.createTerrainMaterial();
    if (doc.state.manifest.value("material", std::string()) == material)
        ImGui::TextWrapped("Uses %s and %s, written on every save", material.c_str(), terrain_texture_path(doc.folder).c_str());
    else
        ImGui::TextDisabled("Bakes these colors into a texture spanning the map");
}

void Panels::drawHeightRangePrompt(Session& session) {
    if (askRange_ && !ImGui::IsPopupOpen("Clip heights?")) ImGui::OpenPopup("Clip heights?");
    if (!ImGui::BeginPopupModal("Clip heights?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%zu samples are outside %.2f .. %.2f m and will be clipped to it.", rangeClips_, rangeMin_, rangeMax_);
    if (ImGui::Button("Apply")) {
        session.setHeightRange(rangeMin_, rangeMax_);
        askRange_ = rangeEdited_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        askRange_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Panels::updateTool(Session& session) {
    const ImGuiIO& io = ImGui::GetIO();
    hover_.reset();
    if (!session.doc) return;
    ZoneDoc& doc = *session.doc;
    const ImVec2 scale = io.DisplayFramebufferScale;
    if (!io.WantCaptureMouse || stroking_) hover_ = view_.pick(doc, io.MousePos.x * scale.x, io.MousePos.y * scale.y);
    const bool click = !io.WantCaptureMouse && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (click && hover_) tile_ = kast::map::tile_of((*hover_)[0], (*hover_)[2]);

    if (tool_ != Tool::Raise && tool_ != Tool::Lower && tool_ != Tool::Flatten) {
        if (stroking_) doc.commit();
        stroking_ = false;
        updateObjects(session, click);
        return;
    }
    brush_.mode = tool_ == Tool::Raise ? BrushMode::Raise : tool_ == Tool::Lower ? BrushMode::Lower : BrushMode::Flatten;
    if (hover_) view_.drawGroundCircle(doc, (*hover_)[0], (*hover_)[2], brush_.radius, kBrushColor);
    drawObjectOverlays(session);
    if (click && hover_ && io.KeyShift && tool_ == Tool::Flatten) {
        brush_.target = (*hover_)[1];
        return;
    }
    if (click && hover_) {
        doc.begin(); // the whole stroke is one undo step
        stroking_ = true;
    }
    if (stroking_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        doc.commit();
        stroking_ = false;
    }
    std::string err;
    const auto info = doc.info(err);
    if (stroking_ && hover_ && info) apply_brush(*info, doc.state, (*hover_)[0], (*hover_)[2], brush_, strength_ * io.DeltaTime);
}

} // namespace zone
