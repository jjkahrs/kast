#pragma once

#include "camera.h"

#include <input.h>
#include <lua_systems.h>
#include <project.h>
#include <scene.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

struct SDL_Window;

enum class PlayState { Stopped, Playing, Paused };
enum class PanelFocus { None, Scene, Game }; // a viewport tab to bring to front

// The scene being edited plus its editor-side state.
struct Document {
    kast::Scene scene;
    EditorCamera camera;
    std::string path; // empty until first saved/opened
    std::optional<kast::Project> project; // the active project; scene systems and kast.loadScene paths are relative to it
    entt::entity selected = entt::null;
    bool dirty = false;

    PlayState play = PlayState::Stopped;
    nlohmann::ordered_json playSnapshot; // the scene as it was when Play started, restored on Stop
    bool playSnapshotDirty = false;
    kast::Input input; // fed by the Game panel while it is focused and playing
    kast::LuaSystems lua; // the scene's Lua systems; a session runs from Play to Stop
    std::string playSceneName; // the running scene's name after a kast.loadScene switch; empty if none happened

    PanelFocus focusRequest = PanelFocus::None; // set by Play/Stop, cleared by the panel that takes focus
};

// Sets dirty, except during Play: play-mode edits are discarded on Stop.
void markDirty(Document& doc);
// Selects the entity with this PersistentId; clears the selection if there is none. Returns whether it was found.
bool selectById(Document& doc, kast::EntityId id);

// Play mode. Play snapshots the scene in memory and runs the systems every frame; Pause holds them; Stop restores
// the snapshot (scene, dirty flag, selection by id) without touching the editor camera. Play from Stopped requests
// Game focus and Stop requests Scene focus; Pause and resume leave focus alone. A kast.loadScene switch replaces
// doc.scene and clears the selection; path and project stay those of the edited scene.
void startPlay(Document& doc); // also resumes when paused
void togglePause(Document& doc);
void stopPlay(Document& doc);
void updatePlay(Document& doc, float dt); // ticks the C++ systems, then the Lua systems, while playing

// doc.path relative to the active project ('/' separators); empty if unsaved or there is no project.
std::string projectScenePath(const Document& doc);
// Lua system list edits (Systems panel). Not allowed during Play; each marks the document dirty. index must be valid.
bool canEditSystems(const Document& doc);
// Stored relative to the project root, '/' separators. False + error (list unchanged) with no project or a script
// outside it.
bool addSystem(Document& doc, const std::filesystem::path& script, std::string& error);
void removeSystem(Document& doc, size_t index);
void moveSystem(Document& doc, size_t index, int delta); // delta -1 = up, +1 = down; no-op past either end
// Writes a template system as <project>/scripts/new_system[_N].lua and adds it. Needs a project and a saved scene.
// False + error if not.
bool newSystemFromTemplate(Document& doc, std::string& error);

// Lua components (Components panel). Definitions hot-reload while stopped and are frozen during Play.
bool canEditComponents(const Document& doc); // a project is active and Play is stopped
// Hot reload: picks up added, removed and modified definition files and migrates doc.scene (marking it dirty if any
// instance data changed). Does nothing during Play, so pending changes apply after Stop, to the restored scene.
void refreshComponents(Document& doc);
// The file New from Template would write: <root>/components/new_component[_N].lua, with type name NewComponent[N].
std::filesystem::path newComponentFile(const std::filesystem::path& projectRoot, std::string& typeName);
// Writes a template definition (one field, each field type shown commented) there and loads it. False + error with no
// project or during Play.
bool newComponentFromTemplate(Document& doc, std::string& error);
// Why Lua components can't be created now (no project, or Play not stopped); null if canEditComponents.
const char* componentEditBlocker(const Document& doc);
// Definition file stem for a type name, snake_case: PlayerHealth -> player_health, HTTPServer2D -> http_server2_d.
std::string componentFileStem(std::string_view typeName);
// <projectRoot>/components/<componentFileStem>.lua for typeName (trimmed): where New Lua Component writes it.
std::filesystem::path luaComponentFile(const std::filesystem::path& projectRoot, std::string_view typeName);
// Why name (trimmed) can't be a new Lua component type (Add Component > New Lua Component...); empty if it can. Checks,
// in order: componentEditBlocker, empty, identifier, built-in name, any listed definition's name (even one in error),
// and an existing luaComponentFile (case-insensitive).
std::string newComponentNameError(const Document& doc, std::string_view name);
// Writes that file from the template with name (trimmed) as the type name, loads it, adds it to entity and marks the
// document dirty. False + error if entity is invalid or newComponentNameError isn't empty (nothing written), the write
// fails, or the definition doesn't load (file kept, nothing added). Never overwrites a file.
bool newLuaComponent(Document& doc, entt::entity entity, std::string_view name, std::string& error);

enum class ObjectKind { Empty, Cube, Plane, Sphere, DirectionalLight, Camera };
constexpr ObjectKind kObjectKinds[] = {ObjectKind::Empty, ObjectKind::Cube, ObjectKind::Plane, ObjectKind::Sphere,
                                       ObjectKind::DirectionalLight, ObjectKind::Camera};
const char* objectKindLabel(ObjectKind kind); // menu label, e.g. "Create Empty", "Cube"

// Adds an object under parent (entt::null = root), selects it and marks the document dirty.
entt::entity createObject(Document& doc, ObjectKind kind, entt::entity parent);
void deleteSelected(Document& doc);
void duplicateSelected(Document& doc);

// Default scene: angled light, ground plane, cube resting on it, a Main Camera and an editor camera framing them.
// Clears path and dirty; keeps the active project.
void resetToDefault(Document& doc);
// Opens a scene; the active project becomes the one found walking up from its directory (maybe none).
bool openFile(Document& doc, const std::string& path, std::string& error); // doc untouched on failure
// Saves without rebasing systems paths; the active project becomes the one found walking up from the saved file.
// Logs a warning if that changes the project of a scene with systems.
bool saveFile(Document& doc, const std::string& path, std::string& error);
// Opens a project.kast: activates it and opens its startScene, or (none, missing or invalid: logged) a new default
// scene. doc untouched if the file is not named project.kast or is invalid.
bool openProject(Document& doc, const std::filesystem::path& file, std::string& error);
// New Project folder name: name trimmed, ASCII letters lower-cased, each whitespace run replaced by '-'.
std::string projectFolderName(std::string_view name);
// The folder New Project would create: dir (made absolute) / projectFolderName(name).
std::filesystem::path newProjectTarget(std::string_view name, const std::string& dir);
// Why New Project can't create name in dir (the first failing rule), or empty if it can. Checks the filesystem.
std::string newProjectError(std::string_view name, const std::string& dir);
// Creates the newProjectTarget folder (never its parents) holding project.kast (name trimmed, startScene null) and
// sets created to it. False + error if newProjectError fails or a write fails; a folder made by this call is removed.
bool createProject(std::string_view name, const std::string& dir, std::filesystem::path& created, std::string& error);
std::string displayName(const Document& doc);
// "Kast Editor - [<project> - ]<scene>[*][ (Playing|Paused[: <switched scene>])]"
std::string windowTitle(const Document& doc);

// File menu behavior: New/Open/Save/Save As with native dialogs, unsaved-changes and error modals, window title.
class FileActions {
public:
    explicit FileActions(Document& doc) : doc_(doc) {}

    void setWindow(SDL_Window* window) { window_ = window; }
    void newScene();
    void open();
    void newProject();
    // Opens the New Project modal prefilled with these values (--new-project); creates nothing by itself.
    void openNewProject(std::string name, std::string dir);
    void openProject();
    void save();
    void saveAs();
    void exit();
    // File actions are available: no dialog or New Project modal open and not in Play mode.
    bool idle() const { return !dialogOpen_ && !newProjectOpen_ && doc_.play == PlayState::Stopped; }

    // Call once per frame inside the ImGui frame: shortcuts, dialog results, modals, title.
    void update();
    // AppHooks::quitRequested: false while the user still has to confirm unsaved changes.
    bool onQuitRequested();

private:
    enum class Pending { None, New, Open, NewProject, OpenProject, Quit };
    enum class DialogKind { Open, SaveAs, NewProjectFolder, OpenProject };

    // Runs action now, or after the unsaved-changes modal if the document is dirty.
    void request(Pending action);
    void perform(Pending action);
    void showSaveDialog(Pending after);
    void handleDialogResult();
    void saveTo(const std::string& path, Pending after);
    void drawModals();
    void drawNewProjectModal();
    void browseNewProjectDir();
    void createNewProject();
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
    std::string newProjectName_; // New Project modal fields
    std::string newProjectDir_;  // parent directory, typed or picked with Browse...
    bool newProjectRequested_ = false;
    bool newProjectOpen_ = false;

    // SDL calls onDialogDone on any thread; the main thread picks the result up in update().
    bool dialogOpen_ = false;
    DialogKind dialogKind_ = DialogKind::Open;
    std::mutex dialogMutex_;
    bool dialogDone_ = false;
    std::string dialogPath_;  // empty = cancelled
    std::string dialogError_; // set if the dialog itself failed
};
