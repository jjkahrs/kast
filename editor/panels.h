#pragma once

#include "document.h"

#include <entt/meta/meta.hpp>

#include <mutex>
#include <string>

// Hierarchy (tree, create/delete/rename/duplicate, drag-drop reparent), Inspector and Systems (the scene's Lua
// systems: list, status while playing, add/remove/reorder, new from template) panels.
class Panels {
public:
    void drawHierarchy(Document& doc);
    void drawInspector(Document& doc);
    void drawSystems(Document& doc);

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
    // One generated Inspector widget for a component field; true if it changed the value.
    bool drawField(entt::entity entity, entt::meta_any& component, const entt::meta_data& field);

    Edit edit_;
    entt::entity renaming_ = entt::null;
    bool focusRename_ = false;
    std::string renameBuffer_;

    // Euler angles shown in the Inspector, kept while editing so they don't jump between equivalent forms.
    entt::entity eulerEntity_ = entt::null;
    const char* eulerField_ = nullptr; // meta field name, a stable string literal
    bx::Quaternion eulerSource_{bx::InitIdentity};
    bx::Vec3 euler_{0.0f, 0.0f, 0.0f};

    // "Add Script..." file dialog: SDL calls back on any thread; drawSystems picks the result up.
    static void onScriptChosen(void* userdata, const char* const* files, int filter);
    std::mutex scriptMutex_;
    bool scriptDialogOpen_ = false;
    bool scriptChosen_ = false;
    std::string scriptPath_; // empty = cancelled
};
