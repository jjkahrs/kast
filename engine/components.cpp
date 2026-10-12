#include "components.h"

#include "scene_io.h"

#include <entt/core/hashed_string.hpp>

#include <format>
#include <stdexcept>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::vector<entt::meta_type>& types()
{
    static std::vector<entt::meta_type> list;
    return list;
}

const char* validateCamera(const entt::meta_any& component)
{
    const auto& camera = component.cast<const Camera&>();
    if (!(camera.verticalFovDegrees > 0.0f && camera.verticalFovDegrees < 180.0f))
        return "\"verticalFovDegrees\" must be between 0 and 180 (exclusive)";
    if (!(camera.nearPlane > 0.0f && camera.nearPlane < camera.farPlane))
        return "must have 0 < \"nearPlane\" < \"farPlane\"";
    return nullptr;
}

void registerBuiltins()
{
    registerComponent<Transform>("Transform", {"Transform", false})
        .data<&Transform::position>("position").custom<FieldInfo>("Position")
        .data<&Transform::rotation>("rotation").custom<FieldInfo>("Rotation")
        .data<&Transform::scale>("scale").custom<FieldInfo>("Scale");

    entt::meta_factory<Primitive>{}
        .type("Primitive")
        .data<Primitive::Cube>("cube").custom<FieldInfo>("Cube")
        .data<Primitive::Plane>("plane").custom<FieldInfo>("Plane")
        .data<Primitive::Sphere>("sphere").custom<FieldInfo>("Sphere");
    registerComponent<MeshRenderer>("MeshRenderer", {"Mesh Renderer"})
        .data<&MeshRenderer::primitive>("primitive").custom<FieldInfo>("Primitive")
        .data<&MeshRenderer::color>("color").custom<FieldInfo>("Color", true);

    registerComponent<DirectionalLight>("DirectionalLight", {"Directional Light"})
        .data<&DirectionalLight::color>("color").custom<FieldInfo>("Color", true)
        .data<&DirectionalLight::intensity>("intensity").custom<FieldInfo>("Intensity");

    registerComponent<Camera>("Camera", {"Camera", true, validateCamera})
        .data<&Camera::verticalFovDegrees>("verticalFovDegrees").custom<FieldInfo>("Vertical FOV")
        .data<&Camera::nearPlane>("nearPlane").custom<FieldInfo>("Near Plane")
        .data<&Camera::farPlane>("farPlane").custom<FieldInfo>("Far Plane");

    registerComponent<Rotator>("Rotator", {"Rotator"})
        .data<&Rotator::axis>("axis").custom<FieldInfo>("Axis")
        .data<&Rotator::degreesPerSecond>("degreesPerSecond").custom<FieldInfo>("Degrees Per Second");
}

// ponytail: not thread-safe; registration happens on the main thread before any scene work.
void ensureBuiltins()
{
    static bool done = false;
    if (!done) {
        done = true; // before registering: registerComponent calls back into addComponentType
        registerBuiltins();
    }
}

Json numbers(std::initializer_list<float> values)
{
    Json out = Json::array();
    for (float v : values)
        out.push_back(jsonNumber(v));
    return out;
}

Json fieldToJson(const entt::meta_any& value)
{
    const entt::meta_type type = value.type();
    if (isType<float>(type))
        return jsonNumber(value.cast<float>());
    if (isType<int>(type))
        return value.cast<int>();
    if (isType<bool>(type))
        return value.cast<bool>();
    if (isType<std::string>(type))
        return value.cast<const std::string&>();
    if (isType<bx::Vec3>(type)) {
        const auto& v = value.cast<const bx::Vec3&>();
        return numbers({v.x, v.y, v.z});
    }
    if (isType<bx::Quaternion>(type)) {
        const auto& q = value.cast<const bx::Quaternion&>();
        return numbers({q.x, q.y, q.z, q.w});
    }
    for (auto [id, constant] : type.data())
        if (constant.get({}) == value)
            return constant.name();
    throw std::logic_error(std::format("component field of unsupported type {}", type.info().name()));
}

template <size_t N>
std::array<float, N> floats(const Json& json)
{
    if (!json.is_array() || json.size() != N)
        throw Invalid(std::format("must be an array of {} numbers", N));
    std::array<float, N> out{};
    for (size_t i = 0; i < N; ++i) {
        if (!json[i].is_number())
            throw Invalid(std::format("must be an array of {} numbers", N));
        out[i] = json[i].get<float>();
    }
    return out;
}

entt::meta_any fieldFromJson(const entt::meta_type& type, const Json& json)
{
    if (isType<float>(type)) {
        if (!json.is_number())
            throw Invalid("must be a number");
        return json.get<float>();
    }
    if (isType<int>(type)) {
        if (!json.is_number_integer())
            throw Invalid("must be an integer");
        return json.get<int>();
    }
    if (isType<bool>(type)) {
        if (!json.is_boolean())
            throw Invalid("must be true or false");
        return json.get<bool>();
    }
    if (isType<std::string>(type)) {
        if (!json.is_string())
            throw Invalid("must be a string");
        return json.get<std::string>();
    }
    if (isType<bx::Vec3>(type)) {
        const auto v = floats<3>(json);
        return bx::Vec3{v[0], v[1], v[2]};
    }
    if (isType<bx::Quaternion>(type)) {
        const auto v = floats<4>(json);
        const bx::Quaternion q{v[0], v[1], v[2], v[3]};
        if (bx::dot(q, q) < 1e-6f)
            throw Invalid("must be a nonzero quaternion");
        return q;
    }
    std::string expected;
    for (auto [id, constant] : type.data()) {
        if (json.is_string() && json.get<std::string>() == constant.name())
            return constant.get({});
        expected += std::format("{}\"{}\"", expected.empty() ? "" : ", ", constant.name());
    }
    throw Invalid(std::format("unknown value {} (expected one of {})", json.dump(), expected));
}

} // namespace

namespace detail {
void addComponentType(const entt::meta_type& type)
{
    ensureBuiltins();
    types().push_back(type);
}
} // namespace detail

const std::vector<entt::meta_type>& componentTypes()
{
    ensureBuiltins();
    return types();
}

const ComponentInfo& componentInfo(const entt::meta_type& type)
{
    return type.custom();
}

entt::meta_type findComponentType(const std::string& name)
{
    for (const entt::meta_type& type : componentTypes())
        if (name == type.name())
            return type;
    return {};
}

bool hasComponent(const entt::registry& registry, entt::entity entity, const entt::meta_type& type)
{
    const entt::sparse_set* pool = registry.storage(type.info().hash());
    return pool && pool->contains(entity);
}

void addComponent(entt::registry& registry, entt::entity entity, const entt::meta_type& type)
{
    componentInfo(type).storage(registry).push(entity); // no value: default-constructs
}

void removeComponent(entt::registry& registry, entt::entity entity, const entt::meta_type& type)
{
    componentInfo(type).storage(registry).remove(entity);
}

entt::meta_any getComponent(entt::registry& registry, entt::entity entity, const entt::meta_type& type)
{
    return type.from_void(componentInfo(type).storage(registry).value(entity));
}

entt::meta_any getComponent(const entt::registry& registry, entt::entity entity, const entt::meta_type& type)
{
    return type.from_void(registry.storage(type.info().hash())->value(entity));
}

void setComponent(entt::registry& registry, entt::entity entity, const entt::meta_any& value)
{
    const entt::meta_type type = value.type();
    entt::sparse_set& pool = componentInfo(type).storage(registry);
    if (pool.contains(entity)) {
        if (!getComponent(registry, entity, type).assign(value))
            throw std::logic_error(std::format("cannot assign component {}", type.name()));
    } else {
        pool.push(entity, value.base().data());
    }
}

void copyComponents(entt::registry& registry, entt::entity source, entt::entity target)
{
    for (const entt::meta_type& type : componentTypes()) {
        if (!hasComponent(registry, source, type))
            continue;
        // Copy by value first (copying a reference meta_any makes an owning copy): adding to the pool may move
        // the source component.
        const entt::meta_any reference = getComponent(std::as_const(registry), source, type);
        const entt::meta_any value = reference;
        setComponent(registry, target, value);
    }
}

Json componentToJson(const entt::meta_any& component)
{
    Json json = Json::object();
    for (auto [id, field] : component.type().data())
        json[field.name()] = fieldToJson(field.get(component));
    return json;
}

entt::meta_any componentFromJson(const entt::meta_type& type, const Json& json, std::string& error)
{
    try {
        if (!json.is_object())
            throw Invalid("must be an object");
        for (const auto& [key, value] : json.items())
            if (!type.data(entt::hashed_string{key.c_str(), key.size()}))
                throw Invalid(std::format("unknown field \"{}\"", key));
        entt::meta_any component = type.construct();
        for (auto [id, field] : type.data()) {
            if (!json.contains(field.name()))
                throw Invalid(std::format("missing field \"{}\"", field.name()));
            try {
                if (!field.set(component, fieldFromJson(field.type(), json[field.name()])))
                    throw std::logic_error(std::format("cannot set field {}", field.name()));
            } catch (const Invalid& e) {
                throw Invalid(std::format("\"{}\" {}", field.name(), e.what()));
            }
        }
        if (const auto validate = componentInfo(type).validate)
            if (const char* reason = validate(component))
                throw Invalid(reason);
        return component;
    } catch (const Invalid& e) {
        error = e.what();
    }
    return {};
}

} // namespace kast
