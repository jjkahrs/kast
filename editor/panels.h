#pragma once

#include "document.h"

#include <components.h>

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>

// Hierarchy (tree, create/delete/rename/duplicate, drag-drop reparent), Inspector, Systems (the scene's Lua
// systems: list, status while playing, add/remove/reorder, new from template) and Components (the project's Lua
// component definitions: list with status, new from template), State Machines (the project's state machine
// definitions: status, Global toggle, live game states, new from template) and Events (the Play session's recently
// delivered events, with a name filter) panels.
class Panels {
public:
    // Opens the Inspector's "New Lua Component" modal (Add Component > New Lua Component...) with name prefilled.
    void openNewComponent(std::string name);
    // Opens the Inspector's Add Component popup once an entity is selected.
    void openAddComponent() { addComponentRequested_ = true; }
    // Opens the State Machines panel's "New State Machine" modal with name prefilled.
    void openNewStateMachine(std::string name);
    void drawHierarchy(Document& doc);
    void drawInspector(Document& doc);
    void drawSystems(Document& doc);
    void drawComponents(Document& doc);
    void drawStateMachines(Document& doc);
    void drawEvents(Document& doc);

private:
    // Tree edits are deferred to the end of drawHierarchy so the child lists are not changed mid-iteration.
    struct Edit {
        enum class Op { None, Create, Delete, Duplicate, Reparent } op = Op::None;
        entt::entity target = entt::null;
        entt::entity parent = entt::null;
        ObjectKind kind = ObjectKind::Empty;
    };

    void drawNode(Document& doc, entt::entity entity);
    void drawCreateItems(entt::entity parent);
    void applyEdit(Document& doc);
    void drawNewComponentModal(Document& doc);
    void drawNewStateMachineModal(Document& doc);
    // The StateMachine component's Inspector body: machine and initial state combos, live state while playing. True if
    // it changed the component.
    bool drawStateMachineComponent(Document& doc, entt::entity entity);
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
};
