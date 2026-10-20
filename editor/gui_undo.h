#pragma once

#include "undo.h"

#include <pugixml.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// GUI Builder undo (docs/editor/undo-redo.html): each edit is a step on the open GUI's history holding a copy of the
// element it touched (with its subtree), before and after, and where it was. Elements are named by their path in the
// document, never by pugi::xml_node (an undo replaces nodes, so handles go stale).

// One element as it was at one moment: its child indices from the document root (text and comments count), and a copy.
struct GuiElementCopy {
    std::vector<size_t> path;
    std::shared_ptr<const pugi::xml_document> copy; // shared: steps are copyable
};

GuiElementCopy copyGuiElement(pugi::xml_node node);
// The node at path under root (a document node); null if there is none.
pugi::xml_node guiNodeAt(pugi::xml_node root, const std::vector<size_t>& path);

// Records on history the step that turned before into after in the document under root: no before = the element was
// added, no after = deleted; both = moved and / or edited. Nothing is recorded (false) if before and after are the
// same. select(node) runs after the step is undone or redone with the element it left (null if it is gone).
bool pushGuiStep(UndoStack& history, pugi::xml_node root, std::string name, std::optional<GuiElementCopy> before,
                 std::optional<GuiElementCopy> after, std::function<void(pugi::xml_node)> select);
