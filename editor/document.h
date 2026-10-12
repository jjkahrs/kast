#pragma once

#include "camera.h"

#include <input.h>
#include <scene.h>

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>

struct SDL_Window;

enum class PlayState { Stopped, Playing, Paused };
enum class PanelFocus { None, Scene, Game }; // a viewport tab to bring to front

// The scene being edited plus its editor-side state.
struct Document {
    kast::Scene scene;
    EditorCamera camera;
    std::string path; // empty until first saved/opened
    entt::entity selected = entt::null;
    bool dirty = false;

    PlayState play = PlayState::Stopped;
    nlohmann::ordered_json playSnapshot; // the scene as it was when Play started, restored on Stop
    bool playSnapshotDirty = false;
    kast::Input input; // fed by the Game panel while it is focused and playing

    PanelFocus focusRequest = PanelFocus::None; // set by Play/Stop, cleared by the panel that takes focus
};

// Sets dirty, except during Play: play-mode edits are discarded on Stop.
void markDirty(Document& doc);
// Selects the entity with this PersistentId; clears the selection if there is none. Returns whether it was found.
bool selectById(Document& doc, kast::EntityId id);

// Play mode. Play snapshots the scene in memory and runs the systems every frame; Pause holds them; Stop restores
// the snapshot (scene, dirty flag, selection by id) without touching the editor camera. Play from Stopped requests
// Game focus and Stop requests Scene focus; Pause and resume leave focus alone.
void startPlay(Document& doc); // also resumes when paused
void togglePause(Document& doc);
void stopPlay(Document& doc);
void updatePlay(Document& doc, float dt); // ticks the systems while playing

enum class ObjectKind { Empty, Cube, Plane, Sphere, DirectionalLight, Camera };
constexpr ObjectKind kObjectKinds[] = {ObjectKind::Empty, ObjectKind::Cube, ObjectKind::Plane, ObjectKind::Sphere,
                                       ObjectKind::DirectionalLight, ObjectKind::Camera};
const char* objectKindLabel(ObjectKind kind); // menu label, e.g. "Create Empty", "Cube"

// Adds an object under parent (entt::null = root), selects it and marks the document dirty.
entt::entity createObject(Document& doc, ObjectKind kind, entt::entity parent);
void deleteSelected(Document& doc);
void duplicateSelected(Document& doc);

// Default scene: angled light, ground plane, cube resting on it, a Main Camera and an editor camera framing them. Clears path and dirty.
void resetToDefault(Document& doc);
bool openFile(Document& doc, const std::string& path, std::string& error); // doc untouched on failure
bool saveFile(Document& doc, const std::string& path, std::string& error);
std::string displayName(const Document& doc);

// File menu behavior: New/Open/Save/Save As with native dialogs, unsaved-changes and error modals, window title.
class FileActions {
public:
    explicit FileActions(Document& doc) : doc_(doc) {}

    void setWindow(SDL_Window* window) { window_ = window; }
    void newScene();
    void open();
    void save();
    void saveAs();
    void exit();
    // File actions are available: no dialog open and not in Play mode.
    bool idle() const { return !dialogOpen_ && doc_.play == PlayState::Stopped; }

    // Call once per frame inside the ImGui frame: shortcuts, dialog results, modals, title.
    void update();
    // AppHooks::quitRequested: false while the user still has to confirm unsaved changes.
    bool onQuitRequested();

private:
    enum class Pending { None, New, Open, Quit };
    enum class DialogKind { Open, SaveAs };

    // Runs action now, or after the unsaved-changes modal if the document is dirty.
    void request(Pending action);
    void perform(Pending action);
    void showSaveDialog(Pending after);
    void handleDialogResult();
    void saveTo(const std::string& path, Pending after);
    void drawModals();
    void updateTitle();

    static void onDialogDone(void* userdata, const char* const* files, int filter);

    Document& doc_;
    SDL_Window* window_ = nullptr;
    Pending pending_ = Pending::None;   // action waiting on the unsaved-changes modal
    Pending afterSave_ = Pending::None; // action to run once a Save As dialog saves successfully
    bool confirmRequested_ = false;
    bool quitConfirmed_ = false;
    std::string error_;
    bool errorRequested_ = false;
    std::string title_;

    // SDL calls onDialogDone on any thread; the main thread picks the result up in update().
    bool dialogOpen_ = false;
    DialogKind dialogKind_ = DialogKind::Open;
    std::mutex dialogMutex_;
    bool dialogDone_ = false;
    std::string dialogPath_;  // empty = cancelled
    std::string dialogError_; // set if the dialog itself failed
};
