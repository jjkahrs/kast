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
    // Removes the models from context (may be null: already gone) and forgets every value.
    void clear(Rml::Context* context);

private:
    void bind(Rml::Context& context, const std::string& model, const std::string& key);

    std::map<std::string, std::map<std::string, GuiValue>> values_; // map nodes never move: RmlUi holds pointers
};

} // namespace kast
