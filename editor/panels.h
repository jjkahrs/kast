#pragma once

#include "assets.h"
#include "document.h"
#include "external_editor.h"
#include "gui_builder.h"

#include <components.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Hierarchy (tree, create/delete/rename/duplicate, drag-drop reparent), Inspector, Systems (the scene's Lua
// systems: list, status while playing, add/remove/reorder, new from template) and Components (the project's Lua
// component definitions: list with status, new from template), State Machines (the project's state machine
// definitions: status, Global toggle, live game states, new from template) and Events (the Play session's recently
// delivered events, with a name filter) and Assets (the project's files: browse, open scenes and GUIs, new folder,
// new Lua system / state machine / GUI, rename, delete to the Recycle Bin, drag-drop move, open text files in an
// external editor) panels.
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
    enum class AssetCreate { System, StateMachine, Gui };
    // Runs Assets Create > Lua System, State Machine or GUI in folder ("" = the project root), as the menu does.
    void requestAssetCreate(AssetCreate what, std::string folder)
    {
        constexpr AssetEdit::Op ops[] = {AssetEdit::Op::NewSystem, AssetEdit::Op::NewStateMachine, AssetEdit::Op::NewGui};
        assetEdit_ = {ops[static_cast<int>(what)], std::move(folder)};
    }
    void drawHierarchy(Document& doc);
    void drawInspector(Document& doc);
    void drawSystems(Document& doc);
    void drawComponents(Document& doc);
    void drawStateMachines(Document& doc);
    void drawEvents(Document& doc);
    // Rescans the project's files for the Assets panel (main's poll). drawAssets also rescans when the project changes.
    void refreshAssets(const Document& doc);
    // Double-clicked .rml files open in gui, .lua/.rcss/.json files in editor; renames and moves of gui's open files
    // reach it.
    void drawAssets(Document& doc, FileActions& files, GuiBuilder& gui, ExternalEditor& editor);

private:
    // Tree edits are deferred to the end of drawHierarchy so the child lists are not changed mid-iteration.
    struct Edit {
        enum class Op { None, Create, Delete, Duplicate, Reparent } op = Op::None;
        entt::entity target = entt::null;
        entt::entity parent = entt::null;
        ObjectKind kind = ObjectKind::Empty;
    };

    // Assets panel file operations, deferred like Edit and applied at the end of drawAssets. Paths are
    // project-relative; "" is the project root.
    struct AssetEdit {
        enum class Op { None, NewFolder, NewSystem, NewStateMachine, NewGui, Rename, Move, Delete } op = Op::None;
        std::string path;   // the entry acted on (NewFolder, NewSystem, NewStateMachine, NewGui: the folder to create in)
        std::string target; // Rename: the new name; Move: the destination folder
    };

    void drawNode(Document& doc, entt::entity entity);
    void drawAssetNode(const Document& doc, FileActions& files, GuiBuilder& gui, ExternalEditor& editor,
                       const AssetEntry& entry, const char* blocker);
    void drawAssetItems(const Document& doc, ExternalEditor& editor, const AssetEntry& entry, const char* blocker);
    void drawAssetCreateMenu(const std::string& folder, const char* blocker); // folder: "" = root
    void openRequestedAssetMenu(const char* popup); // opens popup (the last item's context menu) for openAssetMenu
    void startAssetRename(const std::string& path, const std::string& name);
    void requestAssetDelete(const Document& doc, const std::string& path);
    void applyAssetEdit(Document& doc, GuiBuilder& gui);
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
    // Generated Inspector widgets for a component field's JSON value (built-in or Lua); true if they changed it.
    bool drawField(entt::entity entity, const kast::ComponentType& type, const kast::FieldDesc& field,
                   nlohmann::ordered_json& value);
    // One widget for a single value of field's kind (a whole non-array field or one array element). key identifies
    // the value for the Euler cache.
    bool drawValue(entt::entity entity, const std::string& key, const kast::FieldDesc& field, const char* label,
                   nlohmann::ordered_json& value);

    Edit edit_;
    entt::entity renaming_ = entt::null;
    bool focusRename_ = false;
    std::string renameBuffer_;

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
    AssetEdit assetEdit_;
    std::string assetDeletePath_; // waiting on the delete confirmation
    bool assetDeleteRequested_ = false;
    std::string assetsError_;
    bool assetsErrorRequested_ = false;
};
