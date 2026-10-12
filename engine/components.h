#pragma once

#include "scene.h"

#include <entt/meta/factory.hpp>
#include <entt/meta/meta.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// Data-driven components via entt::meta. A component type registered here (struct + fields) gets JSON save/load,
// copy on duplicate, a generated Inspector UI and Add/Remove Component support without further code.
// Supported field types: float, int, bool, std::string, bx::Vec3, bx::Quaternion and registered enums.
// PersistentId, Name and Hierarchy are structural and not registered.
namespace kast {

// Custom meta data on a component type.
struct ComponentInfo {
    const char* displayName = "";
    bool removable = true;
    // Checks values beyond their field types: returns why the component is invalid, or nullptr. Optional.
    const char* (*validate)(const entt::meta_any& component) = nullptr;
    entt::sparse_set& (*storage)(entt::registry&) = nullptr; // ensures the pool exists; set by registerComponent
};

// Custom meta data on a field or enum constant.
struct FieldInfo {
    const char* displayName = "";
    bool color = false; // bx::Vec3 edited as a color
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
// Copies every registered component of source onto target (overwriting ones target already has).
void copyComponents(entt::registry& registry, entt::entity source, entt::entity target);

// Fields in registration order.
nlohmann::ordered_json componentToJson(const entt::meta_any& component);
// Default-constructed component of type filled from json. Unknown, missing and mistyped fields, unknown enum
// names and values rejected by ComponentInfo::validate are errors: returns an empty meta_any and sets error.
entt::meta_any componentFromJson(const entt::meta_type& type, const nlohmann::ordered_json& json, std::string& error);

} // namespace kast
