#pragma once

#include "gui_access.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace Rml {
class Context;
}

// RmlUi data binding without a schema (docs/engine/game-gui.html, "Data binding"): every data-model an .rml names is
// created on its context before the document loads, and every top-level variable its bindings read is bound to a
// GuiValue held here (an unset one reads as empty), so a document never loads with an unknown-variable warning.
namespace kast {

// What an .rml's bindings name: its data-model names, and the top-level variables its {{...}} and data-* expressions
// read. Left out: data-for / data-alias names, members (a.b), transform names (| f), event callbacks (f(...)), string
// literals and RmlUi's reserved names (it, it_index, ev, true, false, size, literal). Comments are skipped.
struct GuiBindingNames {
    std::set<std::string> models;
    std::set<std::string> variables;
};
GuiBindingNames guiBindingNames(std::string_view rml);

// A virtual list's window (gui_lists.h) on an array of a model: RmlUi reads the variable name as the array of its
// source's rows first .. first + count - 1 (fewer past the source's end; none if it isn't an array). version counts
// the times GuiModels::set replaced the source.
struct GuiWindow {
    std::string name;
    GuiValue* source = nullptr; // a model value (two-way bindings in rows write it)
    int first = 0;
    int count = 0;
    int version = 0;
};

// The data models of one RmlUi context and their values (model -> key -> value). Values outlive the context's models:
// a document reload or a model created later shows them again.
class GuiModels {
public:
    // Before a document of this rml loads into context: creates the models it names and binds the variables it reads
    // (and every key already set on those models).
    void prepare(Rml::Context& context, std::string_view rml);
    // Replaces model.key; if context (may be null) has the model, binds key there and refreshes the views reading it.
    void set(Rml::Context* context, const std::string& model, const std::string& key, GuiValue value);
    std::optional<GuiValue> get(const std::string& model, const std::string& key) const;
    // Removes the models from context (may be null: already gone) and forgets every value and window.
    void clear(Rml::Context* context);
    // A new window on model.source, bound in context's model under a name of its own; null if context has no such
    // model. ponytail: every call binds one more name (RmlUi can't unbind one): a hot reload's lists take new windows,
    // as the old document's rows still read theirs until RmlUi frees them.
    GuiWindow* window(Rml::Context& context, const std::string& model, const std::string& source);
    // Refreshes the views reading the window (after its first or count changed).
    void dirty(Rml::Context& context, const std::string& model, const GuiWindow& window);
    // model.key reads as an empty array until restore(), which puts every held value back and refreshes its views
    // (a virtual list's source while its document loads: its row template's data-for must not make a row per item).
    void hold(const std::string& model, const std::string& key);
    void restore(Rml::Context* context);

private:
    void bind(Rml::Context& context, const std::string& model, const std::string& key);

    std::map<std::string, std::map<std::string, GuiValue>> values_; // map nodes never move: RmlUi holds pointers
    std::map<std::string, std::map<std::string, GuiWindow>> windows_; // model -> name -> window, likewise
    std::map<std::pair<std::string, std::string>, GuiValue> held_;    // {model, key} -> its value while held
};

} // namespace kast
