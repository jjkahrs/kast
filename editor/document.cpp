#include "document.h"

#include <app.h>
#include <log.h>
#include <scene_io.h>
#include <systems.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <filesystem>
#include <format>
#include <fstream>

namespace {

constexpr SDL_DialogFileFilter kSceneFilter{"Kast scene (*.scene)", "scene"};
constexpr bx::Vec3 kNewLightEuler{-50.0f, -30.0f, 0.0f}; // ~50 deg down, slightly from the side so faces shade differently
constexpr bx::Vec3 kMainCameraPosition{0.0f, 3.0f, 7.0f};
constexpr bx::Vec3 kMainCameraEuler{-20.0f, 0.0f, 0.0f}; // looks down at the origin from kMainCameraPosition

constexpr const char* kSystemTemplate = R"(-- A Lua system: update(dt) runs once per frame in Play mode, after the C++ systems.
-- See README "Lua systems" for the full kast.* API.
local elapsed = 0

return {
    -- init = function() kast.log("started") end,
    update = function(dt)
        elapsed = elapsed + dt
        -- for e in kast.query("Transform", "MeshRenderer") do
        --     local t = kast.get(e, "Transform")       -- {position = {x=, y=, z=}, rotation = {x=, y=, z=, w=}, scale = ...}
        --     t.position.y = 0.5 + math.sin(elapsed)
        --     kast.set(e, "Transform", {position = t.position}) -- partial: other fields keep their values
        -- end
        -- if kast.input.keyDown("Space") then kast.log("jump") end
        -- local dx, dy = kast.input.mouseDelta()
        -- local id = kast.create("Spawned"); kast.add(id, "MeshRenderer"); kast.destroy(id)
    end,
    -- shutdown = function() kast.log("stopped") end,
}
)";

// path as a '/'-separated string relative to dir (absolute if they share no root, e.g. another drive).
std::string relativeTo(const std::filesystem::path& path, const std::filesystem::path& dir)
{
    const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
    const std::filesystem::path relative = absolute.lexically_relative(std::filesystem::absolute(dir).lexically_normal());
    return (relative.empty() ? absolute : relative).generic_string();
}

} // namespace

const char* objectKindLabel(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Empty: return "Create Empty";
    case ObjectKind::Cube: return "Cube";
    case ObjectKind::Plane: return "Plane";
    case ObjectKind::Sphere: return "Sphere";
    case ObjectKind::DirectionalLight: return "Directional Light";
    case ObjectKind::Camera: return "Camera";
    }
    return "";
}

entt::entity createObject(Document& doc, ObjectKind kind, entt::entity parent)
{
    kast::Scene& scene = doc.scene;
    const char* name = kind == ObjectKind::Empty ? "Empty" : objectKindLabel(kind);
    const entt::entity entity = scene.create(name, parent);
    switch (kind) {
    case ObjectKind::Cube: scene.registry().emplace<kast::MeshRenderer>(entity, kast::Primitive::Cube); break;
    case ObjectKind::Plane: scene.registry().emplace<kast::MeshRenderer>(entity, kast::Primitive::Plane); break;
    case ObjectKind::Sphere: scene.registry().emplace<kast::MeshRenderer>(entity, kast::Primitive::Sphere); break;
    case ObjectKind::DirectionalLight:
        scene.registry().emplace<kast::DirectionalLight>(entity);
        scene.registry().get<kast::Transform>(entity).rotation = kast::quatFromEulerDeg(kNewLightEuler);
        break;
    case ObjectKind::Camera: scene.registry().emplace<kast::Camera>(entity); break;
    case ObjectKind::Empty: break;
    }
    doc.selected = entity;
    markDirty(doc);
    return entity;
}

void deleteSelected(Document& doc)
{
    if (!doc.scene.valid(doc.selected))
        return;
    doc.scene.destroy(doc.selected);
    doc.selected = entt::null;
    markDirty(doc);
}

void duplicateSelected(Document& doc)
{
    if (!doc.scene.valid(doc.selected))
        return;
    doc.selected = doc.scene.duplicate(doc.selected);
    markDirty(doc);
}

void markDirty(Document& doc)
{
    if (doc.play == PlayState::Stopped)
        doc.dirty = true;
}

bool selectById(Document& doc, kast::EntityId id)
{
    doc.selected = entt::null;
    for (auto [entity, persistent] : doc.scene.registry().view<kast::PersistentId>().each())
        if (persistent.value == id)
            doc.selected = entity;
    return doc.selected != entt::null;
}

void startPlay(Document& doc)
{
    if (doc.play == PlayState::Stopped) {
        doc.playSnapshot = kast::sceneToJson(doc.scene);
        doc.playSnapshotDirty = doc.dirty;
        doc.focusRequest = PanelFocus::Game;
        kast::log::info("Play");
        if (kast::activeCamera(doc.scene) == entt::null)
            kast::log::warn("No Camera in scene: the Game view has nothing to render");
        doc.lua.start(doc.scene, sceneDir(doc), &doc.input);
    }
    doc.play = PlayState::Playing;
}

void togglePause(Document& doc)
{
    if (doc.play != PlayState::Stopped)
        doc.play = doc.play == PlayState::Paused ? PlayState::Playing : PlayState::Paused;
}

void stopPlay(Document& doc)
{
    if (doc.play == PlayState::Stopped)
        return;
    doc.lua.stop(); // shutdowns see the play-mode scene
    const kast::EntityId selected =
        doc.scene.valid(doc.selected) ? doc.scene.registry().get<kast::PersistentId>(doc.selected).value : 0;
    std::string error;
    if (!kast::sceneFromJson(doc.playSnapshot, doc.scene, error))
        kast::log::error(std::format("Cannot restore the scene from before Play: {}", error)); // keeps the play-mode scene
    doc.play = PlayState::Stopped;
    doc.playSnapshot = nullptr;
    doc.dirty = doc.playSnapshotDirty;
    selectById(doc, selected);
    doc.focusRequest = PanelFocus::Scene;
    kast::log::info("Stop");
}

void updatePlay(Document& doc, float dt)
{
    if (doc.play != PlayState::Playing)
        return;
    kast::tickSystems(doc.scene, dt);
    doc.lua.tick(dt);
}

std::filesystem::path sceneDir(const Document& doc)
{
    return doc.path.empty() ? std::filesystem::current_path() : std::filesystem::path(doc.path).parent_path();
}

bool canEditSystems(const Document& doc) { return doc.play == PlayState::Stopped; }

void addSystem(Document& doc, const std::filesystem::path& script)
{
    doc.scene.systems.push_back(relativeTo(script, sceneDir(doc)));
    markDirty(doc);
}

void removeSystem(Document& doc, size_t index)
{
    doc.scene.systems.erase(doc.scene.systems.begin() + static_cast<ptrdiff_t>(index));
    markDirty(doc);
}

void moveSystem(Document& doc, size_t index, int delta)
{
    const ptrdiff_t target = static_cast<ptrdiff_t>(index) + delta;
    if (target < 0 || target >= std::ssize(doc.scene.systems))
        return;
    std::swap(doc.scene.systems[index], doc.scene.systems[static_cast<size_t>(target)]);
    markDirty(doc);
}

bool newSystemFromTemplate(Document& doc, std::string& error)
{
    if (doc.path.empty()) {
        error = "Save the scene first";
        return false;
    }
    const std::filesystem::path dir = sceneDir(doc) / "scripts";
    std::filesystem::path file = dir / "new_system.lua";
    for (int n = 2; std::filesystem::exists(file); ++n)
        file = dir / std::format("new_system_{}.lua", n);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(file, std::ios::binary);
    out << kSystemTemplate;
    out.close();
    if (ec || !out) {
        error = std::format("Cannot write '{}'", file.string());
        return false;
    }
    addSystem(doc, file);
    kast::log::info(std::format("Created Lua system '{}'", file.string()));
    return true;
}

void resetToDefault(Document& doc)
{
    doc.scene = kast::Scene{};
    entt::registry& registry = doc.scene.registry();

    const entt::entity light = createObject(doc, ObjectKind::DirectionalLight, entt::null);
    registry.get<kast::Transform>(light).position = {0.0f, 3.0f, 0.0f};
    registry.get<kast::DirectionalLight>(light).color = {1.0f, 0.96f, 0.9f};

    const entt::entity ground = createObject(doc, ObjectKind::Plane, entt::null);
    registry.get<kast::Name>(ground).value = "Ground";
    registry.get<kast::Transform>(ground).scale = {10.0f, 1.0f, 10.0f};
    registry.get<kast::MeshRenderer>(ground).color = {0.55f, 0.6f, 0.55f};

    const entt::entity cube = createObject(doc, ObjectKind::Cube, entt::null);
    registry.get<kast::Transform>(cube).position = {0.0f, 0.5f, 0.0f}; // unit cube resting on y=0
    registry.get<kast::MeshRenderer>(cube).color = {0.85f, 0.45f, 0.25f};

    const entt::entity camera = createObject(doc, ObjectKind::Camera, entt::null);
    registry.get<kast::Name>(camera).value = "Main Camera";
    registry.get<kast::Transform>(camera).position = kMainCameraPosition;
    registry.get<kast::Transform>(camera).rotation = kast::quatFromEulerDeg(kMainCameraEuler);

    doc.camera = EditorCamera{.pivot = {0.0f, 0.5f, 0.0f}, .yaw = 30.0f, .pitch = -25.0f, .distance = 7.0f};
    doc.path.clear();
    doc.selected = entt::null;
    doc.dirty = false;
}

bool openFile(Document& doc, const std::string& path, std::string& error)
{
    kast::Scene scene;
    if (!kast::loadScene(path, scene, error))
        return false;
    EditorCamera camera;
    if (scene.editorData.contains("camera") && !camera.fromJson(scene.editorData["camera"]))
        kast::log::warn("Scene has an invalid editor camera; using the default view");
    doc.scene = std::move(scene);
    doc.camera = camera;
    doc.path = path;
    doc.selected = entt::null;
    doc.dirty = false;
    kast::log::info(std::format("Opened scene '{}'", path));
    return true;
}

bool saveFile(Document& doc, const std::string& path, std::string& error)
{
    doc.scene.editorData["camera"] = doc.camera.toJson();
    // Save As to another directory: keep the Lua system paths pointing at the same files.
    std::vector<std::string> systems = doc.scene.systems;
    for (std::string& system : doc.scene.systems)
        system = relativeTo(sceneDir(doc) / system, std::filesystem::path(path).parent_path());
    if (!kast::saveScene(doc.scene, path, error)) {
        doc.scene.systems = std::move(systems);
        return false;
    }
    doc.path = path;
    doc.dirty = false;
    kast::log::info(std::format("Saved scene '{}'", path));
    return true;
}

std::string displayName(const Document& doc)
{
    return doc.path.empty() ? "Untitled" : std::filesystem::path(doc.path).stem().string();
}

void FileActions::newScene() { request(Pending::New); }
void FileActions::open() { request(Pending::Open); }
void FileActions::saveAs() { showSaveDialog(Pending::None); }
void FileActions::exit() { kast::requestQuit(); } // same path as the window X, so onQuitRequested runs

void FileActions::save()
{
    if (doc_.path.empty())
        showSaveDialog(Pending::None);
    else
        saveTo(doc_.path, Pending::None);
}

bool FileActions::onQuitRequested()
{
    stopPlay(doc_); // restore the edit-mode scene first, so unsaved-changes checks see it
    if (quitConfirmed_ || (!doc_.dirty && !dialogOpen_))
        return true;
    request(Pending::Quit);
    return false;
}

void FileActions::request(Pending action)
{
    if (dialogOpen_)
        return;
    if (!doc_.dirty) {
        perform(action);
        return;
    }
    pending_ = action;
    confirmRequested_ = true;
}

void FileActions::perform(Pending action)
{
    switch (action) {
    case Pending::New:
        resetToDefault(doc_);
        kast::log::info("New scene");
        break;
    case Pending::Open:
        dialogOpen_ = true;
        dialogKind_ = DialogKind::Open;
        SDL_ShowOpenFileDialog(onDialogDone, this, window_, &kSceneFilter, 1, nullptr, false);
        break;
    case Pending::Quit:
        quitConfirmed_ = true;
        kast::requestQuit();
        break;
    case Pending::None: break;
    }
}

void FileActions::showSaveDialog(Pending after)
{
    if (dialogOpen_)
        return;
    afterSave_ = after;
    dialogOpen_ = true;
    dialogKind_ = DialogKind::SaveAs;
    SDL_ShowSaveFileDialog(onDialogDone, this, window_, &kSceneFilter, 1, doc_.path.empty() ? nullptr : doc_.path.c_str());
}

void FileActions::onDialogDone(void* userdata, const char* const* files, int /*filter*/)
{
    auto* self = static_cast<FileActions*>(userdata);
    std::lock_guard lock(self->dialogMutex_);
    self->dialogDone_ = true;
    self->dialogPath_ = files && files[0] ? files[0] : "";
    self->dialogError_ = files ? "" : SDL_GetError();
}

void FileActions::handleDialogResult()
{
    std::string path;
    std::string dialogError;
    {
        std::lock_guard lock(dialogMutex_);
        if (!dialogDone_)
            return;
        dialogDone_ = false;
        path = std::move(dialogPath_);
        dialogError = std::move(dialogError_);
    }
    dialogOpen_ = false;
    const Pending after = std::exchange(afterSave_, Pending::None);
    if (!path.empty())
        stopPlay(doc_); // the dialog opened before Play started: open/save the edit-mode scene
    if (!dialogError.empty()) {
        error_ = std::format("File dialog failed: {}", dialogError);
    } else if (path.empty()) {
        return; // cancelled
    } else if (dialogKind_ == DialogKind::Open) {
        std::string error;
        if (openFile(doc_, path, error))
            return;
        error_ = error;
    } else {
        if (std::filesystem::path(path).extension().empty())
            path += ".scene";
        saveTo(path, after);
        return;
    }
    kast::log::error(error_);
    errorRequested_ = true;
}

void FileActions::saveTo(const std::string& path, Pending after)
{
    std::string error;
    if (saveFile(doc_, path, error)) {
        perform(after);
        return;
    }
    error_ = error;
    kast::log::error(error_);
    errorRequested_ = true;
}

void FileActions::update()
{
    if (idle()) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, ImGuiInputFlags_RouteGlobal))
            newScene();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
            open();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
            save();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
            saveAs();
    }
    handleDialogResult();
    drawModals();
    updateTitle();
}

void FileActions::drawModals()
{
    if (std::exchange(confirmRequested_, false))
        ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before continuing?", displayName(doc_).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Save")) {
            ImGui::CloseCurrentPopup();
            const Pending action = std::exchange(pending_, Pending::None);
            if (doc_.path.empty())
                showSaveDialog(action);
            else
                saveTo(doc_.path, action);
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            ImGui::CloseCurrentPopup();
            perform(std::exchange(pending_, Pending::None));
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            pending_ = Pending::None;
        }
        ImGui::EndPopup();
    }

    if (std::exchange(errorRequested_, false))
        ImGui::OpenPopup("Error");
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(error_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void FileActions::updateTitle()
{
    const char* play = doc_.play == PlayState::Playing ? " (Playing)" : doc_.play == PlayState::Paused ? " (Paused)" : "";
    std::string title = std::format("Kast Editor - {}{}{}", displayName(doc_), doc_.dirty ? "*" : "", play);
    if (title == title_ || !window_)
        return;
    title_ = std::move(title);
    SDL_SetWindowTitle(window_, title_.c_str());
}
