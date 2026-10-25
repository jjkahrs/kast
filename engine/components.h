#pragma once

#include "scene.h"

#include <entt/meta/factory.hpp>
#include <entt/meta/meta.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Data-driven components via entt::meta. A component type registered here (struct + fields) gets JSON save/load,
// copy on duplicate, a generated Inspector UI and Add/Remove Component support without further code.
// Supported field types: float, int, bool, std::string, std::vector<std::string>, bx::Vec3, bx::Quaternion and
// registered enums.
// PersistentId, Name and Hierarchy are structural and not registered.
namespace kast {

// Custom meta data on a component type.
struct ComponentInfo {
    const char* displayName = "";
    bool removable = true;
    // Checks values beyond their field types: returns why the component is invalid, or nullptr. Optional.
    const char* (*validate)(const entt::meta_any& component) = nullptr;
    bool runtime = false; // made by Play, never authored: left out of Add Component
    entt::sparse_set& (*storage)(entt::registry&) = nullptr; // ensures the pool exists; set by registerComponent
};

// Custom meta data on a field or enum constant.
struct FieldInfo {
    const char* displayName = "";
    bool color = false; // bx::Vec3 edited as a color
    std::optional<double> min, max; // float Inspector clamp; enforce it in ComponentInfo::validate too
    bool optional = false; // missing from JSON = the default (fields added after scenes were saved without them)
    bool omitDefault = false; // optional, and left out of the JSON while it has the default value
};

namespace detail {
void addComponentType(const entt::meta_type& type);
}

// Registers T under a stable type name (the JSON key). Chain .data<&T::field>("key").custom<FieldInfo>(...) for each
// field, in the order they are saved and shown.
template <typename T>
entt::meta_factory<T> registerComponent(const char* name, ComponentInfo info)
{
    info.storage = [](entt::registry& registry) -> entt::sparse_set& { return registry.storage<T>(); };
    entt::meta_factory<T> factory = entt::meta_factory<T>{}.type(name).template custom<ComponentInfo>(info);
    detail::addComponentType(entt::resolve<T>());
    return factory;
}

// True if a field's meta type is T.
template <typename T>
bool isType(const entt::meta_type& type)
{
    return type.info() == entt::type_id<T>();
}

// Registered component types in registration order; registers the built-in ones on first use.
const std::vector<entt::meta_type>& componentTypes();
const ComponentInfo& componentInfo(const entt::meta_type& type);
entt::meta_type findComponentType(const std::string& name); // invalid (falsy) if unknown

bool hasComponent(const entt::registry& registry, entt::entity entity, const entt::meta_type& type);
void addComponent(entt::registry& registry, entt::entity entity, const entt::meta_type& type); // default-constructed
void removeComponent(entt::registry& registry, entt::entity entity, const entt::meta_type& type);
// Reference to the entity's component; the entity must have it.
entt::meta_any getComponent(entt::registry& registry, entt::entity entity, const entt::meta_type& type);
entt::meta_any getComponent(const entt::registry& registry, entt::entity entity, const entt::meta_type& type);
// Adds or overwrites the entity's component with a copy of value.
void setComponent(entt::registry& registry, entt::entity entity, const entt::meta_any& value);
// Copies every registered component, every active Lua component and the PreservedComponents of source onto target
// (overwriting ones target already has).
void copyComponents(entt::registry& registry, entt::entity source, entt::entity target);
// The same between registries (e.g. into or out of a scene kept aside).
void copyComponents(const entt::registry& from, entt::entity source, entt::registry& to, entt::entity target);

// Fields in registration order.
nlohmann::ordered_json componentToJson(const entt::meta_any& component);
// Default-constructed component of type filled from json. Unknown, missing and mistyped fields, unknown enum
// names and values rejected by ComponentInfo::validate are errors: returns an empty meta_any and sets error.
entt::meta_any componentFromJson(const entt::meta_type& type, const nlohmann::ordered_json& json, std::string& error);

// ---------------------------------------------------------------------------------------------------------------------
// Uniform layer over built-in (entt::meta) components and Lua components (data-only types defined by Lua files under
// <project>/components/, see lua_components.h). Lua types have no C++ type, so they can't be meta types: ComponentType
// is a handle to either, FieldDesc describes either's fields, and JSON (the scene-file form) is the common value form.
// Scene IO, the Inspector, Add/Remove Component, Duplicate, Play snapshots and kast.* go through this layer.

enum class FieldKind { Float, Int, Bool, String, Vec3, Quat, Enum };

struct FieldDesc {
    std::string name; // JSON key
    std::string displayName;
    FieldKind kind = FieldKind::Float;
    bool array = false;             // a JSON array of kind (built-ins: std::vector<std::string> only)
    bool color = false;             // Vec3 edited as a color
    std::optional<double> min, max; // Float / Int, per element for arrays (built-ins: Inspector clamp only)
    std::vector<std::string> enumValues; // JSON names
    std::vector<std::string> enumLabels; // Inspector labels, same order
    nlohmann::ordered_json defaultValue; // canonical JSON (Lua types only: built-ins default-construct)
};

struct LuaComponentDef {
    std::string name;        // type name and JSON key
    std::string displayName; // Inspector header and Add Component label
    std::string path;        // project-relative definition file
    std::vector<FieldDesc> fields; // save and Inspector order
};

class ComponentType {
public:
    ComponentType() = default;
    explicit ComponentType(entt::meta_type meta) : meta_(std::move(meta)) {}
    explicit ComponentType(std::shared_ptr<const LuaComponentDef> lua) : lua_(std::move(lua)) {}

    explicit operator bool() const { return static_cast<bool>(meta_) || lua_ != nullptr; }
    const char* name() const;
    const char* displayName() const;
    bool removable() const; // Lua types always are
    const std::vector<FieldDesc>& fields() const;
    const entt::meta_type& meta() const { return meta_; } // falsy for Lua types
    const LuaComponentDef* lua() const { return lua_.get(); } // null for built-ins
    bool operator==(const ComponentType& other) const { return meta_ == other.meta_ && lua_ == other.lua_; }

private:
    entt::meta_type meta_;
    std::shared_ptr<const LuaComponentDef> lua_;
};

// Built-in types in registration order, then the active Lua types sorted by name (also the save and Inspector order).
std::vector<ComponentType> allComponentTypes();
ComponentType findAnyComponentType(std::string_view name); // falsy if neither built-in nor an active Lua type
// Replaces the active Lua types. Callers (lua_components.cpp) guarantee unique names that clash with no built-in or
// structural component, and migrate existing instances themselves.
// ponytail: one global set, like the built-in registry; the editor works on one project at a time.
void setLuaComponentTypes(std::vector<std::shared_ptr<const LuaComponentDef>> defs);
const std::vector<std::shared_ptr<const LuaComponentDef>>& luaComponentTypes();

bool hasComponent(const entt::registry& registry, entt::entity entity, const ComponentType& type);
void addComponent(entt::registry& registry, entt::entity entity, const ComponentType& type); // default value
void removeComponent(entt::registry& registry, entt::entity entity, const ComponentType& type);
// The entity's component as JSON (fields in order); the entity must have it.
nlohmann::ordered_json componentToJson(const entt::registry& registry, entt::entity entity, const ComponentType& type);
// Adds or overwrites the entity's component from JSON with the strict scene-file checks (plus Lua min/max and enum
// values). All-or-nothing: false + error leaves the entity unchanged.
bool setComponentJson(entt::registry& registry, entt::entity entity, const ComponentType& type,
                      const nlohmann::ordered_json& json, std::string& error);

// Lua component instances live in EnTT named runtime storage, one pool per type name ("lua/<name>"), holding the
// canonical JSON. Why: a pool per type keeps them first-class ECS data (has/add/remove/destroy are pool operations, the
// registry drops them with the entity) without a C++ type per component; JSON is their only value form anyway (scene
// files, kast.*, Inspector), so storing it avoids a second variant-based value model.
struct LuaComponentData {
    nlohmann::ordered_json value;
};
entt::id_type luaStorageId(std::string_view name);

// The zero value of a field's kind (0, false, "", zero vec3, identity quat, first enum value) moved into min/max; for
// an array field (unless element) an empty array. Used for omitted defaults and new array elements.
nlohmann::ordered_json zeroFieldValue(const FieldDesc& field, bool element = false);
// The type's default instance: every field's default.
nlohmann::ordered_json defaultLuaComponent(const LuaComponentDef& def);
// One field value (or array element when element is true) checked strictly; returns the canonical JSON (floats
// rounded to float, vectors as arrays). nullopt + error ("must be ...") if it's invalid.
std::optional<nlohmann::ordered_json> luaFieldFromJson(const FieldDesc& field, const nlohmann::ordered_json& json,
                                                       std::string& error, bool element = false);
// Strict: unknown, missing and invalid fields are errors (as componentFromJson). Canonical JSON or nullopt + error.
std::optional<nlohmann::ordered_json> luaComponentFromJson(const LuaComponentDef& def, const nlohmann::ordered_json& json,
                                                           std::string& error);
// Lenient (schema drift): missing fields get defaults, unknown fields are dropped, invalid values are reset to the
// field default. Appends one description per change to fixes; returns the canonical JSON.
nlohmann::ordered_json migrateLuaComponent(const LuaComponentDef& def, const nlohmann::ordered_json& json,
                                           std::vector<std::string>& fixes);

// Components of no known type (no built-in, no valid Lua definition), kept verbatim by name so a missing or broken
// definition never loses scene data. Structural like Name: saved after the known components, copied on Duplicate.
struct PreservedComponents {
    std::map<std::string, nlohmann::ordered_json> byName; // sorted: deterministic save order
};
void removePreservedComponent(entt::registry& registry, entt::entity entity, const std::string& name);

} // namespace kast
