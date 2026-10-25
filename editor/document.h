#pragma once

#include "camera.h"
#include "scene_undo.h"
#include "undo.h"

#include <input.h>
#include <lua_systems.h>
#include <project.h>
#include <scene.h>
#include <shader_clock.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    kast::ShaderClock shaderClock; // u_time of the Scene and Game views: updatePlay advances it, except while paused
    nlohmann::ordered_json playSnapshot; // the scene as it was when Play started, restored on Stop
    bool playSnapshotDirty = false;
    kast::Input input; // fed by the Game panel while it is focused and playing
    kast::LuaSystems lua; // the scene's Lua systems; a session runs from Play to Stop
    std::string playSceneName; // the running scene's name after a kast.loadScene switch; empty if none happened
    uint32_t playGeneration = 0; // bumped when Play starts and on each kast.loadScene switch (Game view GUIs reload)
    // Bumped whenever doc.scene is replaced (Stop, kast.loadScene switch, New, Open): entity handles and in-progress
    // edits held by panels are stale.
    uint32_t sceneGeneration = 0;

    PanelFocus focusRequest = PanelFocus::None; // set by Play/Stop, cleared by the panel that takes focus

    // The scene's edit history (in prefab edit mode: the open prefab's) and the step being recorded (scene_undo.h).
    UndoStack history;
    std::optional<SceneRecording> recording;
    // Bumped when prefab edit mode saves a .prefab: a scene step recorded before then holds instances as their prefab
    // was, so applying it reloads the instances (scene_undo.cpp).
    uint32_t prefabSaves = 0;

    // Prefab edit mode: scene, path, dirty and selection are the open .prefab's (path: its file; saving writes the
    // prefab), and the scene being edited waits here. No Play while it is set.
    struct PrefabEdit {
        kast::Scene scene;
        EditorCamera camera;
        std::string path;
        bool dirty = false;
        kast::EntityId selected = 0;
        std::string prefab; // the open prefab, project-relative
        UndoStack history;  // the scene's, kept while the prefab is open
    };
    std::optional<PrefabEdit> prefabEdit;

    // Called by startPlay once the Lua session has started (its inits ran) and by stopPlay before it stops: main's
    // hand-off of the Online panel's login to kast.auth. Unset: nothing.
    std::function<void(kast::LuaSystems&)> onPlayStarted, onPlayStopping;
};

// Sets dirty, except during Play: play-mode edits are discarded on Stop. Outside a scene step recording
// (beginSceneEdit) the edit can't be undone, so the history's save point is lost.
void markDirty(Document& doc);
// Selects the entity with this PersistentId; clears the selection if there is none. Returns whether it was found.
bool selectById(Document& doc, kast::EntityId id);

// Play mode. Play snapshots the scene in memory and runs the systems every frame; Pause holds them; Stop restores
// the snapshot (scene, dirty flag, selection by id unless a kast.loadScene switch cleared it) without touching the editor camera. Play from Stopped requests
// Game focus and Stop requests Scene focus; Pause and resume leave focus alone. A kast.loadScene switch replaces
// doc.scene and clears the selection; path and project stay those of the edited scene.
void startPlay(Document& doc); // also resumes when paused
void togglePause(Document& doc);
void stopPlay(Document& doc);
// Ticks the C++ systems, then the Lua systems, while playing; in Edit mode only poses animated models (preview).
// Advances doc.shaderClock by dt unless Play is paused.
void updatePlay(Document& doc, float dt);

// doc.path relative to the active project ('/' separators); empty if unsaved or there is no project.
std::string projectScenePath(const Document& doc);
// Lua system list edits (Systems panel). Not allowed during Play; each marks the document dirty. index must be valid.
bool canEditSystems(const Document& doc);
// Stored relative to the project root, '/' separators. False + error (list unchanged) with no project or a script
// outside it.
bool addSystem(Document& doc, const std::filesystem::path& script, std::string& error);
void removeSystem(Document& doc, size_t index);
void moveSystem(Document& doc, size_t index, int delta); // delta -1 = up, +1 = down; no-op past either end
// After an asset rename / move (project-relative from -> to), the in-memory half of the rename pass (remapAssetRefs):
// every asset path (remapAssetPath) in the open scene and the one waiting behind prefab edit mode follows: component
// fields (Lua and preserved ones too), Lua systems, sky faces, the lightmaps, prefab instances; and the open prefab's
// own path. Marks the document dirty and clears its undo history if any changed.
void remapScenePaths(Document& doc, const std::string& from, const std::string& to);
// Writes a template system as <project>/scripts/new_system[_N].lua and adds it. Needs a project and a saved scene.
// False + error if not.
bool newSystemFromTemplate(Document& doc, std::string& error);
// Writes a template system as dir/new_system[_N].lua (creating dir): the first N whose file name is free in dir
// (ignoring case). Sets file to it. False + error if the write fails. Never overwrites a file.
bool writeNewSystem(const std::filesystem::path& dir, std::filesystem::path& file, std::string& error);

// Lua components (Components panel). Definitions hot-reload while stopped and are frozen during Play.
bool canEditComponents(const Document& doc); // a project is active and Play is stopped
// Hot reload: picks up added, removed and modified definition files and migrates doc.scene (marking it dirty and
// clearing the undo history if any instance data changed). Does nothing during Play, so pending changes apply after Stop, to the restored scene.
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
// document dirty (clearing the undo history). False + error if entity is invalid or newComponentNameError isn't empty (nothing written), the write
// fails, or the definition doesn't load (file kept, nothing added). Never overwrites a file.
bool newLuaComponent(Document& doc, entt::entity entity, std::string_view name, std::string& error);

// State machines (State Machines panel, StateMachine Inspector). Definitions are scanned for the editor whenever the
// project or its fsms/ files change; Play loads its own copy (LuaSystems).
void refreshStateMachines(Document& doc); // rescans if the project or its definition files changed
// Why state machines can't be created or made global now (no project, or Play not stopped); null if they can.
const char* stateMachineEditBlocker(const Document& doc);
// <projectRoot>/fsms/<componentFileStem>.lua for name (trimmed): where New State Machine writes it.
std::filesystem::path stateMachineFile(const std::filesystem::path& projectRoot, std::string_view name);
// Why name (trimmed) can't be a new state machine; empty if it can. Checks, in order: stateMachineEditBlocker, empty,
// identifier, any definition file's name (even one in error), and an existing stateMachineFile (case-insensitive).
std::string newStateMachineNameError(const Document& doc, std::string_view name);
// Writes that file from the template with name (trimmed) as the machine name and rescans. False + error if
// newStateMachineNameError isn't empty (nothing written) or the write fails. Never overwrites a file.
bool newStateMachine(Document& doc, std::string_view name, std::string& error);
// dir is <projectRoot>/fsms or inside it (ignoring case): the only place state machine definitions load from.
bool isInStateMachinesDir(const std::filesystem::path& projectRoot, const std::filesystem::path& dir);
// Writes a template state machine as dir/new_state_machine[_N].lua (creating dir), named after the file stem: the first
// N whose file name is free in dir (ignoring case) and whose stem is not in machineNames. Sets file to it. False + error
// if the write fails. Never overwrites a file.
bool writeNewStateMachine(const std::filesystem::path& dir, const std::vector<std::string>& machineNames,
                          std::filesystem::path& file, std::string& error);
// Renames a file written by writeNewStateMachine to newName (renameAsset rules) and, if the stem changes, its machine
// name to the new stem: the template's quoted occurrences of the old stem. False + error (disk unchanged) if newName
// doesn't end in .lua, the new stem isn't an identifier or is in machineNames (the old stem itself is skipped), the
// file no longer holds name = "<old stem>", or the rename or write fails.
bool renameNewStateMachine(const std::filesystem::path& file, const std::string& newName,
                           const std::vector<std::string>& machineNames, std::string& error);
// name is listed in project.kast "stateMachines" (a game machine run in every Play session).
bool isGlobalStateMachine(const Document& doc, const std::string& name);
// Adds name to, or removes it from, project.kast "stateMachines" and saves it (other keys and their order kept). False
// + error (nothing changed) if stateMachineEditBlocker or the write fails.
bool setGlobalStateMachine(Document& doc, const std::string& name, bool global, std::string& error);
// Sets the project's default level for bus and saves project.kast (Mixer panel). Needs a project; false + error if
// there is none or the save fails (the project is unchanged).
bool setBusDefault(Document& doc, kast::AudioBus bus, kast::BusMix level, std::string& error);

enum class ObjectKind {
    Empty, Cube, Plane, Sphere, Model, DirectionalLight, PointLight, SpotLight, AreaLight, ReflectionProbe, Camera,
    ParticleSystem
};
constexpr ObjectKind kObjectKinds[] = {ObjectKind::Empty, ObjectKind::Cube, ObjectKind::Plane, ObjectKind::Sphere,
                                       ObjectKind::Model, ObjectKind::DirectionalLight, ObjectKind::PointLight,
                                       ObjectKind::SpotLight, ObjectKind::AreaLight, ObjectKind::ReflectionProbe,
                                       ObjectKind::Camera, ObjectKind::ParticleSystem};
const char* objectKindLabel(ObjectKind kind); // menu label, e.g. "Create Empty", "Cube"

// Scene edits below are undo steps (scene_undo.h) unless they say they clear the history.
// Adds an object under parent (entt::null = root), selects it and marks the document dirty. Does nothing and returns
// entt::null if parent is at kast::kMaxHierarchyDepth (also createModelEntity).
entt::entity createObject(Document& doc, ObjectKind kind, entt::entity parent);
// Adds an entity named after the model file (a project-relative .glb/.gltf/.fbx/.obj) under parent with a
// ModelRenderer, plus an Animator playing kast::defaultClip if the model has clips; selects it and marks the document
// dirty. What dropping a model from the Assets panel on the viewport or the Hierarchy does.
entt::entity createModelEntity(Document& doc, const std::string& path, entt::entity parent);
void deleteSelected(Document& doc);
void duplicateSelected(Document& doc); // instances inside the copy stay linked (kast::duplicateLinked)

// Prefabs (docs/editor/prefabs.html). Paths are project-relative; each needs a project and Play stopped (false +
// error otherwise, the document unchanged).
// Why prefabs can't be changed now (no project, or Play not stopped); null if they can.
const char* prefabEditBlocker(const Document& doc);
// Writes entity's subtree as <folder>/<entity name>.prefab (uniquePrefabName; folder "" = the project root), makes
// entity an instance of it and selects it. path is the prefab written. Clears the undo history.
bool createPrefabFrom(Document& doc, entt::entity entity, const std::string& folder, std::string& path, std::string& error);
// A new instance of the prefab under parent (entt::null = root), selected. Refused if it would contain the prefab open
// in prefab edit mode.
entt::entity dropPrefab(Document& doc, const std::string& path, entt::entity parent, std::string& error);
// Reloads doc.scene (and the scene waiting behind prefab edit mode) so every instance shows its prefab file as it is
// now, keeping overrides and the selection (by id). Logs failures. Clears the undo histories of scenes holding instances.
void refreshPrefabs(Document& doc);
// refreshPrefabs without touching the histories.
void reloadInstances(Document& doc);
// Enters prefab edit mode on path (or switches to it while already editing one): doc.scene becomes the prefab, with an
// empty undo history (the scene's waits). Unsaved changes of a prefab already open are discarded (FileActions prompts
// first).
bool openPrefab(Document& doc, const std::string& path, std::string& error);
// Leaves prefab edit mode, discarding unsaved prefab changes: the waiting scene comes back with its instances
// refreshed and its undo history. No-op outside edit mode.
void closePrefab(Document& doc);
// An Inspector prefab action (and the --prefab-* flags): on entity (by id); component "Name" means the entity name.
struct PrefabOp {
    enum class Kind { RevertField, RevertComponent, RevertDeleted, RevertAll, ApplyField, ApplyComponent, ApplyDeleted,
                      ApplyAll, Unpack } kind = Kind::RevertAll;
    kast::EntityId entity = 0; // the instance root for *Deleted, *All and Unpack
    std::string component;
    std::string field;
    size_t hop = 0;          // Apply*: the nesting level written (0 = outermost)
    kast::EntityId local = 0; // *Deleted: the deleted child's prefab-local id
};
// Runs op (prefabEditBlocker applies); Apply writes the .prefab files and every instance refreshes. Marks the document
// dirty when it changed. Revert* and Unpack are undo steps; Apply* clears the history. False + error if it couldn't run.
bool runPrefabOp(Document& doc, const PrefabOp& op, std::string& error);
// "<scene name> > <prefab file name>" in prefab edit mode, else displayName.
std::string breadcrumb(const Document& doc);

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
// Creates the newProjectTarget folder (never its parents) holding scenes/main.scene (the resetToDefault scene and its
// editor camera) and project.kast (name trimmed, startScene "scenes/main.scene"), and sets created to it. False +
// error if newProjectError fails or a write fails; a folder made by this call is removed.
bool createProject(std::string_view name, const std::string& dir, std::filesystem::path& created, std::string& error);
std::string displayName(const Document& doc);
// "Kast Editor - [<project> - ]<scene>[*][ (Playing|Paused[: <switched scene>])]"; "*" when the scene or (extraDirty,
// FileActions::extra: the open zone) has unsaved changes.
std::string windowTitle(const Document& doc, bool extraDirty = false);

// File menu behavior: New/Open/Save/Save As with native dialogs, unsaved-changes and error modals, window title.
class FileActions {
public:
    explicit FileActions(Document& doc) : doc_(doc) {}

    void setWindow(SDL_Window* window) { window_ = window; }
    void newScene();
    void open();
    // Opens path (no dialog), after the unsaved-changes prompt if the document is dirty. Failures show the Error modal.
    void openScene(std::string path);
    // Prefab edit mode: opens path (project-relative) / goes back to the scene, after the unsaved-changes prompt if
    // the open prefab is dirty. Opening a scene, New, Build or Quit while a prefab is open leave edit mode first (and
    // then prompt for the scene).
    void openPrefab(std::string path);
    void backFromPrefab();
    void save();
    void saveAs();
    void exit();
    // Build menu: Build... asks for an output folder; Build and Run uses the project's last one (or asks) and then
    // starts the game. Both save or discard unsaved changes first (the prompt), so game.pak holds the on-disk files.
    // Then, if checkStartScene fails, they log why and open Build Settings instead (no folder dialog, no build).
    void build();
    void buildAndRun();
    // Opens the Build Settings modal (Start Scene choice, saved to project.kast); reason is shown in it, may be empty.
    void openBuildSettings(std::string reason = "");
    bool buildSettingsOpen() const { return buildSettingsOpen_; }
    // Opens the Online Services modal (project.online: Enabled, Auth URL, CA file; checkOnlineConfig shown inline, Save
    // writes project.kast via setOnlineConfig).
    void openOnlineSettings();
    bool onlineSettingsOpen() const { return onlineSettingsOpen_; }
    // Opens the Display & GUI Scaling modal (project.gui: reference resolution, match, OS display scale, UI scale and
    // the player's default window; Save writes project.kast via setGuiConfig).
    void openGuiSettings();
    bool guiSettingsOpen() const { return guiSettingsOpen_; }
    // File actions are available: no dialog, unsaved-changes prompt or settings modal open and not in Play mode.
    bool idle() const
    {
        return !dialogOpen_ && pending_ == Pending::None && !buildSettingsOpen_ && !onlineSettingsOpen_ && !guiSettingsOpen_ &&
               doc_.play == PlayState::Stopped;
    }
    // Build menu items are available: idle and a project is open.
    bool canBuild() const { return idle() && doc_.project.has_value(); }

    // Call once per frame inside the ImGui frame: shortcuts, dialog results, modals, title.
    void update();

    // Unsaved work kept outside the scene (main wires the Zone tab's map): the unsaved-changes prompt covers it, Save
    // writes it after the scene, Don't Save drops it. Not while a prefab is open (it belongs to the scene behind it).
    // Unset members: nothing extra.
    struct Extra {
        std::function<std::string()> unsaved; // what has unsaved changes, for the prompt ("maps/meadow"); empty: none
        std::function<bool()> save;           // writes it; false (logged) if it couldn't
        std::function<void()> discard;        // drops the changes
    };
    Extra extra;
    // AppHooks::quitRequested: false while the user still has to confirm unsaved changes.
    bool onQuitRequested();

private:
    enum class Pending { None, New, Open, OpenPath, Quit, Build, BuildAndRun, OpenPrefab, Back };
    enum class DialogKind { Open, SaveAs, BuildFolder, CaFile };

    // extra.unsaved(), or empty (unset, or a prefab is open).
    std::string extraUnsaved() const;
    // The document or extra has unsaved changes.
    bool unsaved() const { return doc_.dirty || !extraUnsaved().empty(); }
    // Runs action now, or after the unsaved-changes modal if unsaved().
    void request(Pending action);
    void perform(Pending action);
    void showSaveDialog(Pending after);
    void handleDialogResult();
    void saveTo(const std::string& path, Pending after);
    void drawModals();
    void drawBuildSettingsModal();
    void drawOnlineSettingsModal();
    void drawGuiSettingsModal();
    // Build / Build and Run: false (Build Settings opened instead) if the project's startScene can't be built.
    bool startSceneReady();
    void browseBuildFolder(bool run);
    void buildTo(const std::filesystem::path& folder, bool run);
    void updateTitle();

    static void onDialogDone(void* userdata, const char* const* files, int filter);

    Document& doc_;
    SDL_Window* window_ = nullptr;
    Pending pending_ = Pending::None;   // action waiting on the unsaved-changes modal
    Pending afterSave_ = Pending::None; // action to run once a Save As dialog saves successfully
    std::string openPath_; // the scene Pending::OpenPath opens
    std::string openPrefab_; // the prefab Pending::OpenPrefab opens
    bool confirmRequested_ = false;
    bool quitConfirmed_ = false;
    std::string error_;
    bool errorRequested_ = false;
    std::string title_;
    bool runAfterBuild_ = false; // the Build folder dialog was opened by Build and Run
    std::vector<std::string> buildScenes_; // Build Settings modal: projectScenes, listed when it opens
    std::string buildStartScene_;          // the Start Scene choice (maybe not in buildScenes_: shown "(missing)")
    std::string buildSettingsError_;
    bool buildSettingsRequested_ = false;
    bool buildSettingsOpen_ = false;
    kast::OnlineConfig online_;        // Online Services modal: the edited copy of project.online
    kast::OnlineConfig onlineChecked_; // the online_ that onlineError_ belongs to (checked again when online_ changes)
    std::string onlineError_;
    bool onlineSettingsRequested_ = false;
    bool onlineSettingsOpen_ = false;
    kast::GuiConfig gui_; // Display & GUI Scaling modal: the edited copy of project.gui
    std::string guiError_;
    bool guiSettingsRequested_ = false;
    bool guiSettingsOpen_ = false;

    // SDL calls onDialogDone on any thread; the main thread picks the result up in update().
    bool dialogOpen_ = false;
    DialogKind dialogKind_ = DialogKind::Open;
    std::mutex dialogMutex_;
    bool dialogDone_ = false;
    std::string dialogPath_;  // empty = cancelled
    std::string dialogError_; // set if the dialog itself failed
};
