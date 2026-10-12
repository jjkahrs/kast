#pragma once

#include "components.h"
#include "scene.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Lua components: data-only component types defined by Lua files under <project>/components/ (scanned recursively),
// one type per file. Once active they behave like built-in components through the uniform layer in components.h.
// See README "Lua components" for the file shape, field types, drift rules and hot reload.
namespace kast {

constexpr const char* kComponentsDir = "components";

// A valid type or field name: letters, digits, _; not starting with a digit.
bool isIdentifier(const std::string& name);
// Taken by a built-in or structural component (PersistentId, Name, ...), so no Lua type may use it. Case-sensitive.
bool isBuiltInComponentName(const std::string& name);

struct ComponentDefinitionStatus {
    std::string path;  // project-relative definition file
    std::string name;  // declared type name; empty if the file didn't get that far
    bool ok = false;
    std::string error; // why it was rejected (starts with path), empty when ok
};

struct ComponentDefinitions {
    std::vector<std::shared_ptr<const LuaComponentDef>> valid;
    std::vector<ComponentDefinitionStatus> status; // one per file, sorted by path
};

// Loads and checks every definition under projectRoot/components/. Changes nothing else: see activate below.
// Each file runs in its own sandboxed environment (lua_sandbox.h) of one fresh Lua state, separate from Play's.
ComponentDefinitions loadComponentDefinitions(const std::filesystem::path& projectRoot);

// Makes root's definitions the active Lua component types (nullopt: none), logs each rejected file as an error and,
// if scene is given, migrates its instances from the previous types (see migrate below). Returns true if any instance
// data changed (callers mark their document dirty).
bool activateComponentDefinitions(const std::optional<std::filesystem::path>& root, Scene* scene);
// Hot reload: if a .lua file under the active project's components/ was added, removed or modified (mtime) since the
// last activation, activates the definitions again and migrates scene. Returns true if scene instance data changed.
// Callers don't call it while playing: definitions stay frozen during Play.
// ponytail: rescans the folder and reloads every definition on any change; fine for tens of files.
bool refreshComponentDefinitions(Scene& scene);
const std::vector<ComponentDefinitionStatus>& componentDefinitionStatus(); // of the last activation
const std::optional<std::filesystem::path>& activeComponentProject();

// Moves scene's component data from the previous Lua types to the active ones: instances of a type that is still
// active get the drift rules (migrateLuaComponent), instances of a type that is gone become PreservedComponents, and
// preserved data whose type is active again is converted with the drift rules. Each drift fix is a Console warning
// naming the entity, component and field. Returns true if any drift fix happened.
bool migrateLuaComponents(Scene& scene, const std::vector<std::shared_ptr<const LuaComponentDef>>& previous);

// "Entity 'Name' (id 3)", for messages.
std::string entityLabel(const entt::registry& registry, entt::entity entity);

} // namespace kast
