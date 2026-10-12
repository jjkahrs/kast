#include "document.h"

#include <app.h>
#include <log.h>
#include <scene_io.h>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <filesystem>
#include <format>

namespace {

constexpr SDL_DialogFileFilter kSceneFilter{"Kast scene (*.scene)", "scene"};
constexpr bx::Vec3 kNewLightEuler{-50.0f, -30.0f, 0.0f}; // ~50 deg down, slightly from the side so faces shade differently

} // namespace

const char* objectKindLabel(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Empty: return "Create Empty";
    case ObjectKind::Cube: return "Cube";
    case ObjectKind::Plane: return "Plane";
    case ObjectKind::Sphere: return "Sphere";
    case ObjectKind::DirectionalLight: return "Directional Light";
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
    case ObjectKind::Empty: break;
    }
    doc.selected = entity;
    doc.dirty = true;
    return entity;
}

void deleteSelected(Document& doc)
{
    if (!doc.scene.valid(doc.selected))
        return;
    doc.scene.destroy(doc.selected);
    doc.selected = entt::null;
    doc.dirty = true;
}

void duplicateSelected(Document& doc)
{
    if (!doc.scene.valid(doc.selected))
        return;
    doc.selected = doc.scene.duplicate(doc.selected);
    doc.dirty = true;
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
    if (!kast::saveScene(doc.scene, path, error))
        return false;
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
    if (!dialogOpen_) {
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
    std::string title = std::format("Kast Editor - {}{}", displayName(doc_), doc_.dirty ? "*" : "");
    if (title == title_ || !window_)
        return;
    title_ = std::move(title);
    SDL_SetWindowTitle(window_, title_.c_str());
}
