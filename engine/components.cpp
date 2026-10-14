#include "components.h"

#include "scene_io.h"

#include <entt/core/hashed_string.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <format>
#include <stdexcept>
#include <unordered_map>

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

std::vector<std::shared_ptr<const LuaComponentDef>>& luaTypes()
{
    static std::vector<std::shared_ptr<const LuaComponentDef>> list; // sorted by name
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

constexpr double kMinPointLightRange = 0.01; // Inspector floor in meters; files and kast.set only need > 0

const char* validatePointLight(const entt::meta_any& component)
{
    const auto& light = component.cast<const PointLight&>();
    for (float channel : {light.color.x, light.color.y, light.color.z})
        if (!(channel >= 0.0f && channel <= 1.0f))
            return "\"color\" channels must be between 0 and 1";
    if (!(light.intensity >= 0.0f))
        return "\"intensity\" must be >= 0";
    if (!(light.range > 0.0f))
        return "\"range\" must be > 0";
    if (!(light.bias >= 0.0f))
        return "\"bias\" must be >= 0";
    return nullptr;
}

const char* validateDirectionalLight(const entt::meta_any& component)
{
    const auto& light = component.cast<const DirectionalLight&>();
    if (!(light.bias >= 0.0f && light.normalBias >= 0.0f))
        return "\"bias\" and \"normalBias\" must be >= 0";
    if (!(light.maxDistance > 0.0f))
        return "\"maxDistance\" must be > 0";
    if (light.cascadeCount < 1 || light.cascadeCount > kMaxShadowCascades)
        return "\"cascadeCount\" must be between 1 and 4"; // kMaxShadowCascades
    return nullptr;
}

const char* validateAnimator(const entt::meta_any& component)
{
    const auto& animator = component.cast<const Animator&>();
    if (!(animator.speed >= 0.0f && animator.speed <= kMaxAnimatorSpeed))
        return "\"speed\" must be between 0 and 10"; // kMaxAnimatorSpeed
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
        .data<&MeshRenderer::material>("material").custom<FieldInfo>("Material")
        .data<&MeshRenderer::castShadows>("castShadows").custom<FieldInfo>(FieldInfo{.displayName = "Cast Shadows", .optional = true})
        .data<&MeshRenderer::receiveShadows>("receiveShadows").custom<FieldInfo>(FieldInfo{.displayName = "Receive Shadows", .optional = true});

    // materials: a JSON array of strings (see FieldDesc::array); the first built-in list field.
    registerComponent<ModelRenderer>("ModelRenderer", {"Model Renderer"})
        .data<&ModelRenderer::model>("model").custom<FieldInfo>("Model")
        .data<&ModelRenderer::materials>("materials").custom<FieldInfo>("Materials")
        .data<&ModelRenderer::castShadows>("castShadows").custom<FieldInfo>(FieldInfo{.displayName = "Cast Shadows", .optional = true})
        .data<&ModelRenderer::receiveShadows>("receiveShadows").custom<FieldInfo>(FieldInfo{.displayName = "Receive Shadows", .optional = true});

    registerComponent<Animator>("Animator", {"Animator", true, validateAnimator})
        .data<&Animator::clip>("clip").custom<FieldInfo>("Clip")
        .data<&Animator::loop>("loop").custom<FieldInfo>("Loop")
        .data<&Animator::speed>("speed").custom<FieldInfo>("Speed", false, 0.0, kMaxAnimatorSpeed)
        .data<&Animator::playOnStart>("playOnStart").custom<FieldInfo>("Play On Start");

    registerComponent<DirectionalLight>("DirectionalLight", {"Directional Light", true, validateDirectionalLight})
        .data<&DirectionalLight::color>("color").custom<FieldInfo>("Color", true)
        .data<&DirectionalLight::intensity>("intensity").custom<FieldInfo>("Intensity")
        .data<&DirectionalLight::castShadows>("castShadows").custom<FieldInfo>(FieldInfo{.displayName = "Cast Shadows", .optional = true})
        .data<&DirectionalLight::bias>("bias").custom<FieldInfo>(FieldInfo{.displayName = "Shadow Bias", .min = 0.0, .optional = true})
        .data<&DirectionalLight::normalBias>("normalBias").custom<FieldInfo>(FieldInfo{.displayName = "Shadow Normal Bias", .min = 0.0, .optional = true})
        .data<&DirectionalLight::maxDistance>("maxDistance").custom<FieldInfo>(FieldInfo{.displayName = "Shadow Distance", .min = 0.1, .optional = true})
        .data<&DirectionalLight::cascadeCount>("cascadeCount")
        .custom<FieldInfo>(FieldInfo{.displayName = "Shadow Cascades", .min = 1.0, .max = double(kMaxShadowCascades), .optional = true});

    registerComponent<PointLight>("PointLight", {"Point Light", true, validatePointLight})
        .data<&PointLight::color>("color").custom<FieldInfo>("Color", true)
        .data<&PointLight::intensity>("intensity").custom<FieldInfo>("Intensity", false, 0.0)
        .data<&PointLight::range>("range").custom<FieldInfo>("Range", false, kMinPointLightRange)
        .data<&PointLight::castShadows>("castShadows").custom<FieldInfo>(FieldInfo{.displayName = "Cast Shadows", .optional = true})
        .data<&PointLight::bias>("bias").custom<FieldInfo>(FieldInfo{.displayName = "Shadow Bias", .min = 0.0, .optional = true});

    registerComponent<Camera>("Camera", {"Camera", true, validateCamera})
        .data<&Camera::verticalFovDegrees>("verticalFovDegrees").custom<FieldInfo>("Vertical FOV")
        .data<&Camera::nearPlane>("nearPlane").custom<FieldInfo>("Near Plane")
        .data<&Camera::farPlane>("farPlane").custom<FieldInfo>("Far Plane");

    registerComponent<Rotator>("Rotator", {"Rotator"})
        .data<&Rotator::axis>("axis").custom<FieldInfo>("Axis")
        .data<&Rotator::degreesPerSecond>("degreesPerSecond").custom<FieldInfo>("Degrees Per Second");

    registerComponent<StateMachine>("StateMachine", {"State Machine"})
        .data<&StateMachine::machine>("machine").custom<FieldInfo>("Machine")
        .data<&StateMachine::initialState>("initialState").custom<FieldInfo>("Initial State");

    registerComponent<UIDocument>("UIDocument", {"UI Document"})
        .data<&UIDocument::path>("path").custom<FieldInfo>("Path")
        .data<&UIDocument::visible>("visible").custom<FieldInfo>("Visible");
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
    if (isType<std::vector<std::string>>(type))
        return value.cast<const std::vector<std::string>&>();
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

float finiteFloat(double value)
{
    const auto f = static_cast<float>(value);
    if (!std::isfinite(f))
        throw Invalid("must be a finite number within float range");
    return f;
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
        out[i] = finiteFloat(json[i].get<double>());
    }
    return out;
}

// Normalized, so a hand-edited [2,0,0,0] doesn't scale the mesh. Already-unit values (to float rounding) are kept
// bit-exact so save/load round trips stay byte-identical.
bx::Quaternion unitQuaternion(const Json& json)
{
    const auto v = floats<4>(json);
    const bx::Quaternion q{v[0], v[1], v[2], v[3]};
    const float lengthSquared = bx::dot(q, q);
    if (!(lengthSquared >= 1e-6f && std::isfinite(lengthSquared)))
        throw Invalid("must be a nonzero quaternion");
    return std::abs(lengthSquared - 1.0f) > 1e-5f ? bx::normalize(q) : q;
}

entt::meta_any fieldFromJson(const entt::meta_type& type, const Json& json)
{
    if (isType<float>(type)) {
        if (!json.is_number())
            throw Invalid("must be a number");
        return finiteFloat(json.get<double>());
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
    if (isType<std::vector<std::string>>(type)) {
        if (!json.is_array() || !std::ranges::all_of(json, [](const Json& e) { return e.is_string(); }))
            throw Invalid("must be an array of strings");
        return json.get<std::vector<std::string>>();
    }
    if (isType<bx::Vec3>(type)) {
        const auto v = floats<3>(json);
        return bx::Vec3{v[0], v[1], v[2]};
    }
    if (isType<bx::Quaternion>(type))
        return unitQuaternion(json);
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
    for (const auto& def : luaTypes()) {
        auto& pool = registry.storage<LuaComponentData>(luaStorageId(def->name));
        if (!pool.contains(source))
            continue;
        LuaComponentData value = pool.get(source); // copy first: emplacing may move the source
        if (pool.contains(target))
            pool.get(target) = std::move(value);
        else
            pool.emplace(target, std::move(value));
    }
    if (const auto* preserved = registry.try_get<PreservedComponents>(source)) {
        PreservedComponents value = *preserved;
        registry.emplace_or_replace<PreservedComponents>(target, std::move(value));
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
            if (!json.contains(field.name())) {
                const auto* info = static_cast<const FieldInfo*>(field.custom());
                if (info && info->optional)
                    continue;
                throw Invalid(std::format("missing field \"{}\"", field.name()));
            }
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

// --- Uniform layer ---

namespace {

std::vector<FieldDesc> describeBuiltin(const entt::meta_type& type)
{
    std::vector<FieldDesc> fields;
    for (auto [id, data] : type.data()) {
        FieldDesc field;
        field.name = data.name();
        const auto* info = static_cast<const FieldInfo*>(data.custom());
        field.displayName = info ? info->displayName : field.name;
        field.color = info && info->color;
        if (info) {
            field.min = info->min;
            field.max = info->max;
        }
        const entt::meta_type t = data.type();
        if (isType<float>(t)) {
            field.kind = FieldKind::Float;
        } else if (isType<int>(t)) {
            field.kind = FieldKind::Int;
        } else if (isType<bool>(t)) {
            field.kind = FieldKind::Bool;
        } else if (isType<std::string>(t)) {
            field.kind = FieldKind::String;
        } else if (isType<std::vector<std::string>>(t)) {
            field.kind = FieldKind::String;
            field.array = true;
        } else if (isType<bx::Vec3>(t)) {
            field.kind = FieldKind::Vec3;
        } else if (isType<bx::Quaternion>(t)) {
            field.kind = FieldKind::Quat;
        } else if (t.is_enum()) {
            field.kind = FieldKind::Enum;
            for (auto [constantId, constant] : t.data()) {
                field.enumValues.emplace_back(constant.name());
                field.enumLabels.emplace_back(static_cast<const FieldInfo&>(constant.custom()).displayName);
            }
        } else {
            throw std::logic_error(std::format("component field of unsupported type {}", t.info().name()));
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

void checkRange(const FieldDesc& field, double value)
{
    if (field.min && value < *field.min)
        throw Invalid(std::format("must be >= {}", *field.min));
    if (field.max && value > *field.max)
        throw Invalid(std::format("must be <= {}", *field.max));
}

Json scalarFromJson(const FieldDesc& field, const Json& json)
{
    switch (field.kind) {
    case FieldKind::Float: {
        if (!json.is_number())
            throw Invalid("must be a number");
        const float value = finiteFloat(json.get<double>());
        checkRange(field, value);
        return jsonNumber(value);
    }
    case FieldKind::Int: {
        if (!json.is_number_integer())
            throw Invalid("must be an integer");
        const bool fits = json.is_number_unsigned() ? json.get<uint64_t>() <= INT_MAX
                                                    : json.get<int64_t>() >= INT_MIN && json.get<int64_t>() <= INT_MAX;
        if (!fits)
            throw Invalid("must be a 32-bit integer");
        const int value = json.get<int>();
        checkRange(field, value);
        return value;
    }
    case FieldKind::Bool:
        if (!json.is_boolean())
            throw Invalid("must be true or false");
        return json;
    case FieldKind::String:
        if (!json.is_string())
            throw Invalid("must be a string");
        return json;
    case FieldKind::Vec3: {
        const auto v = floats<3>(json);
        return numbers({v[0], v[1], v[2]});
    }
    case FieldKind::Quat: {
        const bx::Quaternion q = unitQuaternion(json);
        return numbers({q.x, q.y, q.z, q.w});
    }
    case FieldKind::Enum: {
        std::string expected;
        for (const std::string& value : field.enumValues) {
            if (json.is_string() && json.get<std::string>() == value)
                return json;
            expected += std::format("{}\"{}\"", expected.empty() ? "" : ", ", value);
        }
        throw Invalid(std::format("unknown value {} (expected one of {})", json.dump(), expected));
    }
    }
    throw std::logic_error("unknown field kind");
}

// The canonical value of a field (or one array element); throws Invalid with the reason, without the field name.
Json fieldFromJson(const FieldDesc& field, const Json& json, bool element)
{
    if (!field.array || element)
        return scalarFromJson(field, json);
    if (!json.is_array())
        throw Invalid("must be an array");
    Json out = Json::array();
    for (size_t i = 0; i < json.size(); ++i) {
        try {
            out.push_back(scalarFromJson(field, json[i]));
        } catch (const Invalid& e) {
            throw Invalid(std::format("element {} {}", i + 1, e.what()));
        }
    }
    return out;
}

bool knownField(const LuaComponentDef& def, const std::string& name)
{
    return std::ranges::any_of(def.fields, [&](const FieldDesc& f) { return f.name == name; });
}

} // namespace

const char* ComponentType::name() const { return lua_ ? lua_->name.c_str() : meta_.name(); }

const char* ComponentType::displayName() const
{
    return lua_ ? lua_->displayName.c_str() : componentInfo(meta_).displayName;
}

bool ComponentType::removable() const { return lua_ ? true : componentInfo(meta_).removable; }

const std::vector<FieldDesc>& ComponentType::fields() const
{
    if (lua_)
        return lua_->fields;
    static std::unordered_map<entt::id_type, std::vector<FieldDesc>> cache; // node-based: references stay valid
    auto it = cache.find(meta_.id());
    if (it == cache.end())
        it = cache.emplace(meta_.id(), describeBuiltin(meta_)).first;
    return it->second;
}

std::vector<ComponentType> allComponentTypes()
{
    std::vector<ComponentType> out;
    for (const entt::meta_type& type : componentTypes())
        out.emplace_back(type);
    for (const auto& def : luaTypes())
        out.emplace_back(def);
    return out;
}

ComponentType findAnyComponentType(std::string_view name)
{
    if (const entt::meta_type type = findComponentType(std::string(name)))
        return ComponentType(type);
    for (const auto& def : luaTypes())
        if (def->name == name)
            return ComponentType(def);
    return {};
}

void setLuaComponentTypes(std::vector<std::shared_ptr<const LuaComponentDef>> defs)
{
    std::ranges::sort(defs, {}, [](const auto& def) { return def->name; });
    luaTypes() = std::move(defs);
}

const std::vector<std::shared_ptr<const LuaComponentDef>>& luaComponentTypes() { return luaTypes(); }

entt::id_type luaStorageId(std::string_view name)
{
    std::string key = "lua/";
    key += name;
    return entt::hashed_string::value(key.c_str(), key.size());
}

bool hasComponent(const entt::registry& registry, entt::entity entity, const ComponentType& type)
{
    if (!type.lua())
        return hasComponent(registry, entity, type.meta());
    const auto* pool = registry.storage<LuaComponentData>(luaStorageId(type.name()));
    return pool && pool->contains(entity);
}

void addComponent(entt::registry& registry, entt::entity entity, const ComponentType& type)
{
    if (!type.lua())
        return addComponent(registry, entity, type.meta());
    auto& pool = registry.storage<LuaComponentData>(luaStorageId(type.name()));
    if (!pool.contains(entity))
        pool.emplace(entity, LuaComponentData{defaultLuaComponent(*type.lua())});
}

void removeComponent(entt::registry& registry, entt::entity entity, const ComponentType& type)
{
    if (!type.lua())
        return removeComponent(registry, entity, type.meta());
    registry.storage<LuaComponentData>(luaStorageId(type.name())).remove(entity);
}

Json componentToJson(const entt::registry& registry, entt::entity entity, const ComponentType& type)
{
    if (!type.lua())
        return componentToJson(getComponent(registry, entity, type.meta()));
    return registry.storage<LuaComponentData>(luaStorageId(type.name()))->get(entity).value;
}

bool setComponentJson(entt::registry& registry, entt::entity entity, const ComponentType& type, const Json& json,
                      std::string& error)
{
    if (!type.lua()) {
        const entt::meta_any value = componentFromJson(type.meta(), json, error);
        if (!value)
            return false;
        setComponent(registry, entity, value);
        return true;
    }
    std::optional<Json> value = luaComponentFromJson(*type.lua(), json, error);
    if (!value)
        return false;
    auto& pool = registry.storage<LuaComponentData>(luaStorageId(type.name()));
    if (pool.contains(entity))
        pool.get(entity).value = std::move(*value);
    else
        pool.emplace(entity, LuaComponentData{std::move(*value)});
    return true;
}

Json zeroFieldValue(const FieldDesc& field, bool element)
{
    if (field.array && !element)
        return Json::array();
    const double lowest = field.min.value_or(-HUGE_VAL);
    const double zero = std::clamp(0.0, lowest, std::max(lowest, field.max.value_or(HUGE_VAL)));
    switch (field.kind) {
    case FieldKind::Float: return jsonNumber(static_cast<float>(zero));
    case FieldKind::Int: return static_cast<int>(zero);
    case FieldKind::Bool: return false;
    case FieldKind::String: return "";
    case FieldKind::Vec3: return numbers({0.0f, 0.0f, 0.0f});
    case FieldKind::Quat: return numbers({0.0f, 0.0f, 0.0f, 1.0f});
    case FieldKind::Enum: return field.enumValues.empty() ? Json("") : Json(field.enumValues.front());
    }
    return nullptr;
}

Json defaultLuaComponent(const LuaComponentDef& def)
{
    Json json = Json::object();
    for (const FieldDesc& field : def.fields)
        json[field.name] = field.defaultValue;
    return json;
}

std::optional<Json> luaFieldFromJson(const FieldDesc& field, const Json& json, std::string& error, bool element)
{
    try {
        return fieldFromJson(field, json, element);
    } catch (const Invalid& e) {
        error = e.what();
    }
    return std::nullopt;
}

std::optional<Json> luaComponentFromJson(const LuaComponentDef& def, const Json& json, std::string& error)
{
    try {
        if (!json.is_object())
            throw Invalid("must be an object");
        for (const auto& [key, value] : json.items())
            if (!knownField(def, key))
                throw Invalid(std::format("unknown field \"{}\"", key));
        Json out = Json::object();
        for (const FieldDesc& field : def.fields) {
            if (!json.contains(field.name))
                throw Invalid(std::format("missing field \"{}\"", field.name));
            try {
                out[field.name] = fieldFromJson(field, json[field.name], false);
            } catch (const Invalid& e) {
                throw Invalid(std::format("\"{}\" {}", field.name, e.what()));
            }
        }
        return out;
    } catch (const Invalid& e) {
        error = e.what();
    }
    return std::nullopt;
}

Json migrateLuaComponent(const LuaComponentDef& def, const Json& json, std::vector<std::string>& fixes)
{
    if (!json.is_object()) {
        fixes.push_back(std::format("{} is not an object; reset to defaults", json.dump()));
        return defaultLuaComponent(def);
    }
    for (const auto& [key, value] : json.items())
        if (!knownField(def, key))
            fixes.push_back(std::format("unknown field \"{}\" dropped", key));
    Json out = Json::object();
    for (const FieldDesc& field : def.fields) {
        if (!json.contains(field.name)) {
            fixes.push_back(std::format("missing field \"{}\" set to default {}", field.name, field.defaultValue.dump()));
            out[field.name] = field.defaultValue;
            continue;
        }
        try {
            out[field.name] = fieldFromJson(field, json[field.name], false);
        } catch (const Invalid& e) {
            fixes.push_back(std::format("\"{}\" {} {}; reset to default {}", field.name, json[field.name].dump(), e.what(),
                                        field.defaultValue.dump()));
            out[field.name] = field.defaultValue;
        }
    }
    return out;
}

void removePreservedComponent(entt::registry& registry, entt::entity entity, const std::string& name)
{
    auto* preserved = registry.try_get<PreservedComponents>(entity);
    if (!preserved)
        return;
    preserved->byName.erase(name);
    if (preserved->byName.empty())
        registry.remove<PreservedComponents>(entity);
}

} // namespace kast
