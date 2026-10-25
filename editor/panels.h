#pragma once

#include "assets.h"
#include "document.h"
#include "external_editor.h"
#include "gui_builder.h"
#include "model_preview.h"
#include "shader_graph_editor.h"
#include "zone_session.h"

#include <components.h>
#include <material.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

// Hierarchy (tree, create/delete/rename/duplicate, drag-drop reparent), Inspector, Systems (the scene's Lua
// systems: list, status while playing, add/remove/reorder, new from template) and Components (the project's Lua
// component definitions: list with status, new from template), State Machines (the project's state machine
// definitions: status, Global toggle, live game states, new from template) and Events (the Play session's recently
// delivered events, with a name filter) and Assets (the project's files: browse, open scenes and GUIs, new folder,
// new Lua system / state machine / GUI, rename, delete to the Recycle Bin, drag-drop move, open text files in an
// external editor), Lighting (the scene's ambient light and sky), Post Processing (the scene's post effects and fog)
// and Mixer (the project's default bus levels, live voices during Play) panels.
class Panels {
public:
    // Opens the Inspector's "New Lua Component" modal (Add Component > New Lua Component...) with name prefilled.
    void openNewComponent(std::string name);
    // Opens the Inspector's Add Component popup once an entity is selected.
    void openAddComponent() { addComponentRequested_ = true; }
    // Opens the State Machines panel's "New State Machine" modal with name prefilled.
    void openNewStateMachine(std::string name);
    // Opens the Assets context menu of the entry at path ("" = the panel background) with its Create submenu open, for
    // screenshots. The entry must be listed at the top level or inside an open folder.
    void openAssetMenu(std::string path) { assetMenuRequest_ = std::move(path); }
    enum class AssetCreate { System, StateMachine, Gui, Material, Shader, ShaderGraph };
    // Runs Assets Create > Lua System, State Machine, GUI, Material, Shader or Shader Graph in folder ("" = the project
    // root), as the menu does.
    void requestAssetCreate(AssetCreate what, std::string folder)
    {
        constexpr AssetEdit::Op ops[] = {AssetEdit::Op::NewSystem, AssetEdit::Op::NewStateMachine, AssetEdit::Op::NewGui,
                                         AssetEdit::Op::NewMaterial, AssetEdit::Op::NewShader, AssetEdit::Op::NewShaderGraph};
        assetEdit_ = {ops[static_cast<int>(what)], std::move(folder)};
    }
    // Renames the Assets entry at path (project-relative) to name, as its Rename field does. --assets-rename.
    void requestAssetRename(std::string path, std::string name)
    {
        assetEdit_ = {AssetEdit::Op::Rename, std::move(path), std::move(name)};
    }
    // Selects the Assets entry at path (project-relative), as a click does: a .mat or .kshader also shows in the
    // Inspector until an entity is selected. Opens the entry's folder.
    void selectAsset(const std::string& path);
    // The Inspector's material edits (each saved to its .mat), while it shows a material; else null.
    UndoStack* materialHistory() { return hasMaterial() ? &materialHistory_ : nullptr; }
    // Sets one field or .kshader param (kast::setMaterialProperty) of the material in the Inspector as its widgets do:
    // saved, one undo step. False + error if none is shown or the value is invalid. --material-set.
    bool editMaterial(const std::string& field, const nlohmann::ordered_json& value, std::string& error);
    // Opens the Assets "Delete Asset" confirmation for path (project-relative), as Delete does.
    void openAssetDelete(const Document& doc, const std::string& path) { requestAssetDelete(doc, path); }
    // Opens the selected entity's Inspector context menu of field ("Component.field") with its Apply submenu, for
    // screenshots.
    void openApplyMenu(std::string field) { applyMenuRequest_ = std::move(field); }
    // The Assets folder new prefabs go to (Hierarchy > Create Prefab): the selected folder, else the selected file's
    // folder; "" = the project root.
    std::string assetFolder() const;
    // files: the prefab edit mode breadcrumb's Back.
    void drawHierarchy(Document& doc, FileActions& files);
    void drawInspector(Document& doc);
    void drawSystems(Document& doc);
    void drawComponents(Document& doc);
    void drawStateMachines(Document& doc);
    void drawEvents(Document& doc);
    // The scene's ambient light, sky, lightmaps (settings, Bake Lightmaps, its progress and Cancel, its status) and Bake
    // All Probes (Window > Lighting); draws nothing while open is false. skyError: why the Scene view's cubemap sky fell
    // back to the procedural one (Renderer::skyError), shown under the faces.
    void drawLighting(Document& doc, bool& open, const std::string& skyError);
    // The scene's lightmap bake as the host's renderer sees it (Renderer::lightmapStatus: "Not baked", "Baked", "Out of
    // date" or why its files can't be used), and the running bake's progress (nullopt: none). Set by the host.
    std::function<std::string(const kast::Scene&)> lightmapStatus;
    std::function<std::optional<float>()> lightmapProgress;
    // Whether Bake Lightmaps / Cancel was pressed since the last call: the host starts or stops its LightmapJob.
    bool takeLightmapBakeRequest() { return std::exchange(lightmapBakeRequested_, false); }
    bool takeLightmapCancelRequest() { return std::exchange(lightmapCancelRequested_, false); }
    // A ReflectionProbe's bake as the host's renderer sees its files ("Not baked", "Baked" or why they can't be used:
    // Renderer::probeStatus), shown in its Inspector section. Set by the host.
    std::function<std::string(const kast::ReflectionProbe&)> probeStatus;
    // The LOD level the host's Scene view drew an entity at (Renderer::lodLevel: -1 culled, nullopt not drawn), shown
    // live in its LOD Group Inspector section. Set by the host.
    std::function<std::optional<int>(const kast::Scene&, entt::entity)> lodLevel;
    // The probes the Inspector's Bake or the Lighting panel's Bake All Probes asked for since the last call: the host
    // bakes them between frames (SceneViewport::bakeProbes).
    std::vector<entt::entity> takeBakeRequests() { return std::exchange(bakeRequests_, {}); }
    // Every ReflectionProbe of the scene, as Bake All Probes asks (--bake-probes).
    void requestBakeAll(const Document& doc);
    // A VoxelGI volume's bake as the host's renderer sees it (Renderer::voxelGiStatus: "Not baked", "Baked", "Out of
    // date" or why its files can't be used), and the running VoxelGI bake's progress (nullopt: none). Set by the host.
    std::function<std::string(const kast::Scene&, entt::entity)> voxelGiStatus;
    std::function<std::optional<float>()> voxelGiProgress;
    // The volumes a VoxelGI's Inspector Bake (one) or the Lighting panel's Bake VoxelGI (empty: all) asked for since the
    // last call, and whether Cancel was pressed: the host starts or stops its VoxelGiJob.
    std::optional<std::vector<entt::entity>> takeVoxelGiBakeRequest() { return std::exchange(voxelGiBakeRequest_, std::nullopt); }
    bool takeVoxelGiCancelRequest() { return std::exchange(voxelGiCancelRequested_, false); }
    // The scene's post-processing and fog, a section per effect (Window > Post Processing); nothing while open is false.
    void drawPostProcessing(Document& doc, bool& open);
    // Test hook (--scroll-post-section): the Post Processing panel keeps the section whose name starts with this at its
    // top, for screenshots.
    std::string scrollPostSection;
    // The scene's physics settings: gravity (Window > Physics); draws nothing while open is false.
    void drawPhysics(Document& doc, bool& open);
    // The project's default bus volumes and mutes, saved in project.kast (Window > Mixer); during Play also the live
    // voices. Draws nothing while open is false.
    void drawMixer(Document& doc, bool& open);
    // Read-only, during Play: the session's kast.net connections (a card each: type, address, state, bytes, last
    // error) and in-flight HTTP requests (Window > Network). Draws nothing while open is false.
    void drawNetwork(const Document& doc, bool& open);
    // Rescans the project's files for the Assets panel (main's poll). drawAssets also rescans when the project changes.
    void refreshAssets(const Document& doc);
    // Double-clicked .rml files open in gui, .kshadergraph files in graphs, models in preview, .lua/.rcss/.json files in
    // editor; renames and moves of their open files reach them, and of any asset the open zone's refs.
    void drawAssets(Document& doc, FileActions& files, GuiBuilder& gui, ShaderGraphEditor& graphs, ModelPreview& preview,
                    ExternalEditor& editor, ZoneSession& zone);

private:
    // Tree edits are deferred to the end of drawHierarchy so the child lists are not changed mid-iteration.
    struct Edit {
        enum class Op { None, Create, CreateModel, CreatePrefab, DropPrefab, Delete, Duplicate, Reparent } op = Op::None;
        entt::entity target = entt::null;
        entt::entity parent = entt::null;
        ObjectKind kind = ObjectKind::Empty;
        std::string path; // CreateModel, DropPrefab: the dropped file
    };

    // Assets panel file operations, deferred like Edit and applied at the end of drawAssets. Paths are
    // project-relative; "" is the project root.
    struct AssetEdit {
        enum class Op { None, NewFolder, NewSystem, NewStateMachine, NewGui, NewMaterial, NewShader, NewShaderGraph, NewPrefab, Rename, Move, Delete } op = Op::None;
        std::string path;   // the entry acted on (NewFolder and New*: the folder to create in)
        std::string target; // Rename: the new name; Move: the destination folder
        entt::entity entity = entt::null; // NewPrefab: the entity dropped from the Hierarchy
    };

    // After doc.scene was replaced: drops the Hierarchy rename and the active ImGui item (an edit in progress).
    void forgetStaleScene(const Document& doc);
    void drawNode(Document& doc, entt::entity entity);
    // The Inspector's prefab card (link, Revert All / Apply All / Unpack, removed components, deleted and added
    // children) for an instance entity or a missing-prefab placeholder.
    void drawPrefabCard(Document& doc, entt::entity entity, const nlohmann::ordered_json& overrides);
    // After the last Inspector item (a field, a component header or the Name): marks it overridden and gives it a
    // Revert / Apply context menu. field empty: the whole component.
    void drawOverrideMenu(Document& doc, entt::entity entity, const std::string& component, const std::string& field,
                          bool overridden);
    void drawAssetNode(const Document& doc, FileActions& files, GuiBuilder& gui, ShaderGraphEditor& graphs, ModelPreview& preview,
                       ExternalEditor& editor, const AssetEntry& entry, const char* blocker);
    void drawAssetItems(const Document& doc, ExternalEditor& editor, const AssetEntry& entry, const char* blocker);
    void drawAssetCreateMenu(const std::string& folder, const char* blocker); // folder: "" = root
    void openRequestedAssetMenu(const char* popup); // opens popup (the last item's context menu) for openAssetMenu
    void startAssetRename(const std::string& path, const std::string& name);
    void requestAssetDelete(const Document& doc, const std::string& path);
    void applyAssetEdit(Document& doc, GuiBuilder& gui, ShaderGraphEditor& graphs, ModelPreview& preview, ZoneSession& zone);
    void assetsFailed(std::string error); // logs it and shows the Assets Error modal
    void drawCreateItems(entt::entity parent);
    void applyEdit(Document& doc);
    void drawNewComponentModal(Document& doc);
    void drawNewStateMachineModal(Document& doc);
    // The StateMachine component's Inspector body: machine and initial state combos, live state while playing. True if
    // it changed the component.
    bool drawStateMachineComponent(Document& doc, entt::entity entity);
    // The UIDocument component's Inspector body: a combo of the project's .rml files and Visible. True if it changed.
    bool drawUIDocumentComponent(Document& doc, entt::entity entity);
    // MeshRenderer "material": a combo of the project's .mat files and None (default). True if it changed value.
    bool drawMaterialPicker(Document& doc, nlohmann::ordered_json& value);
    // ModelRenderer "model": a combo of the project's model files.
    bool drawModelPicker(Document& doc, nlohmann::ordered_json& value);
    // ModelRenderer "materials": a .mat combo per slot of the entity's model ("None (embedded)" = the model's own).
    bool drawSlotMaterialPickers(Document& doc, entt::entity entity, nlohmann::ordered_json& value);
    // Animator "clip": a combo of the model's clips.
    bool drawClipPicker(Document& doc, entt::entity entity, nlohmann::ordered_json& value);
    // AudioSource "clip": a combo of the project's .wav / .mp3 files; an unreadable clip shows its error.
    bool drawSoundPicker(Document& doc, nlohmann::ordered_json& value);
    // Preview / Stop: plays the AudioSource's clip in edit mode at its volume and bus (disabled during Play).
    void drawAudioPreview(Document& doc, entt::entity entity);
    void stopPreview();
    // Play/Pause and a time scrub on the Animator's runtime state: poses the model, never saved, never dirties.
    void drawAnimatorPreview(Document& doc, entt::entity entity);
    void drawParticlePreview(Document& doc, entt::entity entity);
    // A ReflectionProbe's Inspector section: its bake status and the Bake button.
    void drawProbeBake(Document& doc, entt::entity entity);
    // A VoxelGI's Inspector section: its bake status (logged when it changes), the Bake button or the running bake's
    // progress and Cancel.
    void drawVoxelGiBake(Document& doc, entt::entity entity);
    // The status line of a bake ("Not baked" dimmed, "Baked", "Out of date" highlighted, else an error).
    static void drawBakeStatus(const std::string& status);
    // The Inspector for the .mat in inspectedAsset_: every edit saves the file (the renderers reload it) and is an undo
    // step on materialHistory_. Its Shader section picks the .kshader, shows its compile status and its params.
    void drawMaterialInspector(Document& doc);
    // The Inspector for the .kshader or .kshadergraph in inspectedAsset_: compile status (errors with .kshader line
    // numbers, a graph's on its nodes) and params; a graph also gets "Open in Shader Graph".
    void drawShaderInspector(Document& doc);
    bool hasMaterial() const; // the Inspector shows a .mat
    // Generated Inspector widgets for a component field's JSON value (built-in or Lua); true if they changed it.
    bool drawField(entt::entity entity, const kast::ComponentType& type, const kast::FieldDesc& field,
                   nlohmann::ordered_json& value);
    // One widget for a single value of field's kind (a whole non-array field or one array element). key identifies
    // the value for the Euler cache.
    bool drawValue(entt::entity entity, const std::string& key, const kast::FieldDesc& field, const char* label,
                   nlohmann::ordered_json& value);

    Edit edit_;
    std::optional<PrefabOp> prefabOp_; // an Inspector prefab action, run at the end of drawInspector
    std::string applyMenuRequest_;     // openApplyMenu
    entt::entity renaming_ = entt::null;
    bool focusRename_ = false;
    std::string renameBuffer_;
    uint32_t sceneGeneration_ = 0; // the Document::sceneGeneration last drawn

    // Euler angles shown in the Inspector, kept while editing so they don't jump between equivalent forms.
    entt::entity eulerEntity_ = entt::null;
    std::string eulerField_; // "<component>.<field>[<element>]"
    bx::Quaternion eulerSource_{bx::InitIdentity};
    bx::Vec3 euler_{0.0f, 0.0f, 0.0f};

    // "Add Script..." file dialog: SDL calls back on any thread; drawSystems picks the result up.
    static void onScriptChosen(void* userdata, const char* const* files, int filter);
    std::mutex scriptMutex_;
    bool scriptDialogOpen_ = false;
    bool scriptChosen_ = false;
    std::string scriptPath_; // empty = cancelled
    std::string systemsError_; // shown in the Systems panel's error modal
    bool systemsErrorRequested_ = false;

    bool addComponentRequested_ = false;
    std::vector<entt::entity> bakeRequests_; // takeBakeRequests
    bool lightmapBakeRequested_ = false, lightmapCancelRequested_ = false;
    std::string lightmapStatus_; // the Lighting panel's last status, logged when it changes
    std::optional<std::vector<entt::entity>> voxelGiBakeRequest_; // takeVoxelGiBakeRequest
    bool voxelGiCancelRequested_ = false;
    std::map<std::string, std::string> voxelGiStatus_; // each volume's (by name) last status shown, logged when it changes
    bool newComponentRequested_ = false;
    std::string newComponentName_;
    std::string newComponentError_; // why the last Create failed (write or load); cleared when the name changes
    std::string eventsFilter_;      // Events panel: shows events whose name contains this
    bool newStateMachineRequested_ = false;
    std::string newStateMachineName_;
    std::string newStateMachineError_; // why the last Create failed; cleared when the name changes

    std::filesystem::path assetsRoot_; // the project root assets_ was scanned from; empty = no project
    std::vector<AssetEntry> assets_;
    std::string assetSelected_; // empty = none
    std::string assetRenaming_; // empty = none
    bool focusAssetRename_ = false;
    std::string assetRenameBuffer_;
    std::optional<std::string> assetMenuRequest_; // openAssetMenu: the entry whose menu opens next frame
    bool assetCreateMenuRequested_ = false;       // open the Create submenu when it is next drawn
    std::string assetNewFsm_; // a just-created state machine: its first rename (if any) also renames the machine
    std::string assetExpand_; // a folder to expand next frame (New Folder's parent)
    bool assetScrollTo_ = false; // selectAsset: scroll the selected entry into view when next drawn
    AssetEdit assetEdit_;
    std::string assetDeletePath_; // waiting on the delete confirmation
    std::vector<std::string> assetDeleteUsers_; // the scenes and prefabs with instances of prefabs it would delete
    bool assetDeleteRequested_ = false;
    std::string assetsError_;
    bool assetsErrorRequested_ = false;

    kast::Audio preview_;                // AudioSource Preview, separate from the Play session
    kast::AudioHandle previewHandle_ = 0; // the playing preview; stale once it ends
    std::map<std::string, std::string> clipErrors_; // AudioSource clips checked since the last Assets scan: why each can't play ("" = ok)

    // The Inspector shows inspectedAsset_ (a project-relative .mat, .kshader or .kshadergraph, set by selecting it in Assets) instead of the
    // selected entity until another entity is selected.
    std::string inspectedAsset_;
    std::string openGraph_; // a .kshadergraph the Inspector's "Open in Shader Graph" asked for (drawAssets opens it)
    entt::entity inspectedEntity_ = entt::null; // doc.selected when last drawn
    struct {
        std::string path; // what value / error were loaded from
        std::optional<std::filesystem::file_time_type> mtime;
        kast::Material value;
        std::string error; // why it can't be edited; empty when value is valid
    } material_;
    UndoStack materialHistory_; // every material's Inspector edits: each step saves its .mat
    std::optional<kast::Material> materialBefore_; // the material before the edit gesture in progress
};
