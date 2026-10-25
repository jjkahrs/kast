#pragma once

#include <pugixml.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// GUI prefabs (docs/engine/game-gui.html, "GUI prefabs"), no RmlUi needed. A <stem>.guiprefab file holds exactly one XML
// element (children allowed): the prefab. An optional sibling <stem>.rcss holds its styles. A GUI document places an
// instance with a reference element naming the prefab by project path, plus its overrides:
//   <div id="hp" data-prefab="ui/hp_bar.guiprefab">
//       <override target="title" text="Health"/>                    the text of the prefab's element with id "title"
//       <override target="title" attribute="class" value="big"/>    an attribute of it (no value="": removed)
//       <override target="" attribute="style" value="width: 50%;"/> target "": the prefab's root element
//       <override target="1/0" text="..."/>                         an element by child indices from the root
//   </div>
// Expanding a reference replaces it with a copy of the prefab's element: its id is the reference's (required), every id
// inside it becomes "<id>_<id inside>" (cloneElement's convention), references inside the prefab are expanded first
// (nested prefabs: "<outer>_<inner>_<id>"), then the overrides are applied, and data-prefab="<path>" stays on it. Other
// attributes and children of the reference are ignored. A reference that can't be expanded (no id; the prefab missing,
// unreadable or not one element; a cycle: a prefab holding itself, directly or nested) is logged with log::warn naming
// the document and the path and stays as an empty placeholder element; the rest of the document expands as usual.
namespace kast {

constexpr const char* kGuiPrefabExtension = ".guiprefab";
constexpr const char* kGuiPrefabAttribute = "data-prefab";
constexpr const char* kGuiOverrideTag = "override";
// On a reference left unexpanded when GuiPrefabExpansion::keepPlaceholders is set (the GUI Builder): never saved.
constexpr const char* kGuiMissingPrefabAttribute = "data-kast-missing";
// How the engine parses .rml text it expands (and saves it back with format_raw | format_no_escapes): entities, white
// space and <style> sheets stay exactly as written for RmlUi.
constexpr unsigned kGuiRawParseOptions = pugi::parse_cdata | pugi::parse_ws_pcdata | pugi::parse_comments;

// A project file's text by project-relative path; nullopt if it can't be read.
using GuiPrefabReader = std::function<std::optional<std::string>(const std::string& path)>;

struct GuiPrefabExpansion {
    std::string document; // the document's project path, named in warnings
    GuiPrefabReader read;
    unsigned parseOptions = kGuiRawParseOptions; // how prefab files are parsed (the document's own way)
    bool quiet = false;            // no warnings (the GUI Builder's own re-expansions)
    bool keepPlaceholders = false; // a reference that can't be expanded keeps its overrides, marked kGuiMissingPrefabAttribute
    std::vector<std::string> files;  // out: every prefab file read and every prefab .rcss found (for hot reload)
    std::vector<std::string> sheets; // out: the .rcss of the prefabs expanded, once each, in first-use order
    std::string error;               // out: why the last reference that failed did
};

// "ui/x.guiprefab" -> "ui/x.rcss".
std::string guiPrefabStylesheet(std::string_view prefab);
// Expands reference (an element with data-prefab) in place. Returns the expanded element, or reference itself if it
// can't be expanded (logged unless quiet; expansion.error says why).
pugi::xml_node guiExpandPrefab(pugi::xml_node reference, GuiPrefabExpansion& expansion);
// Expands every reference under node (node itself excluded).
void guiExpandPrefabs(pugi::xml_node node, GuiPrefabExpansion& expansion);
// rml with every reference expanded and a <link type="text/rcss"> to each prefab sheet put in its <head> before the
// document's own links (so the document's rules win at equal specificity), href relative to the document. Returned
// unchanged if it holds no reference or isn't well-formed (RmlUi reports that when it loads it).
std::string guiExpandPrefabsRml(std::string_view rml, GuiPrefabExpansion& expansion);
// The markup of a new instance of the prefab at path with id (kast.gui.instantiate); nullopt if it can't be expanded
// (expansion.error says why; nothing logged).
std::optional<std::string> guiPrefabInstanceRml(const std::string& path, const std::string& id, GuiPrefabExpansion& expansion);
// Every id in the markup's elements (kast.gui.instantiate checks none is taken).
std::vector<std::string> guiMarkupIds(std::string_view rml);

} // namespace kast
