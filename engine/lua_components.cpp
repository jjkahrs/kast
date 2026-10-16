#include "lua_components.h"

#include "files.h"
#include "log.h"
#include "lua_sandbox.h"
#include "project.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <stdexcept>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;
using Files = std::map<std::filesystem::path, std::filesystem::file_time_type>;

using Invalid = InvalidDefinition;

constexpr const char* kStructuralNames[] = {"PersistentId", "Name", "Hierarchy", "PreservedComponents"};
constexpr std::pair<const char*, FieldKind> kKinds[] = {
    {"float", FieldKind::Float}, {"int", FieldKind::Int},   {"bool", FieldKind::Bool}, {"string", FieldKind::String},
    {"vec3", FieldKind::Vec3},   {"quat", FieldKind::Quat}, {"enum", FieldKind::Enum},
};
constexpr const char* kScalarKinds = "float, int, bool, string, vec3, quat, enum";

struct ActiveState {
    std::optional<std::filesystem::path> root;
    Files files;
    std::vector<ComponentDefinitionStatus> status;
};

ActiveState& active()
{
    static ActiveState state;
    return state;
}

// Every .lua file under root/components/ with its modification time; empty if there is no such folder.
Files scan(const std::filesystem::path& root)
{
    Files files;
    std::error_code ec;
    for (const std::filesystem::path& file : listFiles(root / kComponentsDir))
        if (file.extension() == ".lua")
            files[file] = std::filesystem::last_write_time(file, ec); // disk only: under a pak, never changes
    return files;
}

} // namespace

bool isIdentifier(const std::string& name)
{
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !name.empty() && alpha(name[0]) &&
           std::ranges::all_of(name, [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

bool isBuiltInComponentName(const std::string& name)
{
    return std::ranges::any_of(kStructuralNames, [&](const char* n) { return name == n; }) ||
           static_cast<bool>(findComponentType(name));
}

namespace {

std::string identifier(const sol::table& table, const char* key, const std::string& what)
{
    const sol::object value = table[key];
    if (value.get_type() != sol::type::string || !isIdentifier(value.as<std::string>()))
        throw Invalid(std::format("{}\"{}\" must be a name (letters, digits, _; not starting with a digit)", what, key));
    return value.as<std::string>();
}

std::optional<double> bound(const sol::table& table, const char* key, const FieldDesc& field, const std::string& what)
{
    const sol::object value = table[key];
    if (value.get_type() == sol::type::lua_nil)
        return std::nullopt;
    if (field.kind != FieldKind::Float && field.kind != FieldKind::Int)
        throw Invalid(std::format("{}\"{}\" is only allowed on float and int fields", what, key));
    if (value.get_type() != sol::type::number || !std::isfinite(value.as<double>()))
        throw Invalid(std::format("{}\"{}\" must be a finite number", what, key));
    const double number = value.as<double>();
    if (field.kind == FieldKind::Int && number != std::floor(number))
        throw Invalid(std::format("{}\"{}\" must be an integer on an int field", what, key));
    return number;
}

FieldDesc parseField(lua_State* L, const sol::object& object, size_t index, std::set<std::string>& names)
{
    std::string what = std::format("fields[{}]: ", index);
    if (object.get_type() != sol::type::table)
        throw Invalid(std::format("{}must be a table {{name = ..., type = ...}}", what));
    const sol::table table = object.as<sol::table>();
    FieldDesc field;
    field.name = identifier(table, "name", what);
    what = std::format("fields[{}] (\"{}\"): ", index, field.name);
    if (!names.insert(field.name).second)
        throw Invalid(std::format("{}duplicate field name", what));
    checkKeys(table, {"name", "type", "of", "values", "min", "max", "color", "default"}, what);
    field.displayName = field.name;

    const auto kindOf = [&](const char* key) {
        const sol::object type = table[key];
        if (type.get_type() == sol::type::string)
            for (const auto& [name, kind] : kKinds)
                if (type.as<std::string>() == name)
                    return kind;
        const bool of = std::string_view(key) == "of";
        throw Invalid(std::format("{}unknown \"{}\" {} (expected one of {}{})", what, key, toString(L, type), kScalarKinds,
                                  of ? "; arrays can't nest" : ", array"));
    };
    const sol::object type = table["type"];
    field.array = type.get_type() == sol::type::string && type.as<std::string>() == "array";
    if (field.array)
        field.kind = kindOf("of");
    else if (sol::object of = table["of"]; of.get_type() != sol::type::lua_nil)
        throw Invalid(std::format("{}\"of\" is only allowed on array fields", what));
    else
        field.kind = kindOf("type");

    const sol::object values = table["values"];
    if (field.kind == FieldKind::Enum) {
        for (const sol::object& value : sequence(values, what + "\"values\"")) {
            if (value.get_type() != sol::type::string || value.as<std::string>().empty())
                throw Invalid(std::format("{}\"values\" must be non-empty strings", what));
            if (std::ranges::find(field.enumValues, value.as<std::string>()) != field.enumValues.end())
                throw Invalid(std::format("{}duplicate value \"{}\" in \"values\"", what, value.as<std::string>()));
            field.enumValues.push_back(value.as<std::string>());
        }
        if (field.enumValues.empty())
            throw Invalid(std::format("{}\"values\" must not be empty", what));
        field.enumLabels = field.enumValues;
    } else if (values.get_type() != sol::type::lua_nil) {
        throw Invalid(std::format("{}\"values\" is only allowed on enum fields", what));
    }

    field.min = bound(table, "min", field, what);
    field.max = bound(table, "max", field, what);
    if (field.min && field.max && *field.min > *field.max)
        throw Invalid(std::format("{}\"min\" must not be greater than \"max\"", what));

    const sol::object color = table["color"];
    if (color.get_type() != sol::type::lua_nil) {
        if (field.kind != FieldKind::Vec3 || color.get_type() != sol::type::boolean)
            throw Invalid(std::format("{}\"color\" must be true or false, on vec3 fields only", what));
        field.color = color.as<bool>();
    }

    const sol::object value = table["default"];
    Json json = zeroFieldValue(field);
    std::string error;
    if (value.get_type() != sol::type::lua_nil && !luaToFieldJson(field, value, json, error))
        throw Invalid(std::format("{}default: {}", what, error));
    const std::optional<Json> canonical = luaFieldFromJson(field, json, error);
    if (!canonical)
        throw Invalid(std::format("{}default {} {}", what, json.dump(), error));
    field.defaultValue = *canonical;
    return field;
}

std::shared_ptr<LuaComponentDef> parseDefinition(sol::state& lua, const std::filesystem::path& file,
                                                 const std::string& path, std::string& declared)
{
    std::string error;
    sol::environment env = sandboxEnvironment(lua);
    sol::protected_function body = loadSandboxed(lua, file, "@" + path, env, error);
    if (!body.valid())
        throw Invalid(error.starts_with("cannot open") ? std::format("{}: {}", path, error) : error);
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state());
        return body();
    }();
    if (!result.valid())
        throw Invalid(result.get<sol::error>().what()); // "path:line: message"
    if (result.return_count() != 1 || result.get_type() != sol::type::table)
        throw Invalid(std::format("{}: must return exactly one table {{name = ..., fields = {{...}}}}", path));
    const sol::table table = result;
    checkPlainData(table, path + ": ");
    auto def = std::make_shared<LuaComponentDef>();
    def->path = path;
    def->name = identifier(table, "name", path + ": ");
    declared = def->name;
    // persist and replicate are world-server keys (services/world README "Lua components and systems"), accepted here
    // so client and server can share definition files. The engine checks them and otherwise ignores them: it applies
    // whatever the server replicates.
    checkKeys(table, {"name", "displayName", "fields", "persist", "replicate"}, path + ": ");
    if (const sol::object persist = table["persist"];
        persist.get_type() != sol::type::lua_nil && persist.get_type() != sol::type::boolean)
        throw Invalid(std::format("{}: \"persist\" must be true or false", path));
    if (const sol::object replicate = table["replicate"]; replicate.get_type() != sol::type::lua_nil) {
        const std::string mode = replicate.get_type() == sol::type::string ? replicate.as<std::string>() : "";
        if (mode != "none" && mode != "owner" && mode != "aoi")
            throw Invalid(std::format("{}: \"replicate\" must be \"none\", \"owner\" or \"aoi\", got {}", path,
                                      toString(lua.lua_state(), replicate)));
    }
    const sol::object displayName = table["displayName"];
    if (displayName.get_type() == sol::type::lua_nil)
        def->displayName = def->name;
    else if (displayName.get_type() == sol::type::string && !displayName.as<std::string>().empty())
        def->displayName = displayName.as<std::string>();
    else
        throw Invalid(std::format("{}: \"displayName\" must be a non-empty string", path));
    std::set<std::string> names;
    const std::vector<sol::object> fields = sequence(table["fields"], path + ": \"fields\"");
    for (size_t i = 0; i < fields.size(); ++i) {
        try {
            def->fields.push_back(parseField(lua.lua_state(), fields[i], i + 1, names));
        } catch (const Invalid& e) {
            throw Invalid(std::format("{}: {}", path, e.what()));
        }
    }
    return def;
}

void warnFixes(const entt::registry& registry, entt::entity entity, const std::string& component,
               const std::vector<std::string>& fixes)
{
    for (const std::string& fix : fixes)
        log::warn(std::format("{}: {}: {}", entityLabel(registry, entity), component, fix));
}

} // namespace

std::string entityLabel(const entt::registry& registry, entt::entity entity)
{
    return std::format("Entity '{}' (id {})", registry.get<Name>(entity).value, registry.get<PersistentId>(entity).value);
}

ComponentDefinitions loadComponentDefinitions(const std::filesystem::path& projectRoot)
{
    ComponentDefinitions out;
    sol::state lua;
    openSandbox(lua);
    std::vector<std::shared_ptr<LuaComponentDef>> parsed; // parallel to out.status
    for (const auto& [file, mtime] : scan(projectRoot)) {
        ComponentDefinitionStatus status;
        status.path = toProjectPath(projectRoot, file).value_or(file.generic_string());
        std::shared_ptr<LuaComponentDef> def;
        try {
            def = parseDefinition(lua, file, status.path, status.name);
            status.ok = true;
        } catch (const Invalid& e) {
            status.error = e.what();
        } catch (const std::exception& e) { // sol2 conversions
            status.error = std::format("{}: {}", status.path, e.what());
        }
        out.status.push_back(std::move(status));
        parsed.push_back(std::move(def));
    }

    // Name clashes. Lua-vs-Lua rejects every file of a name, so the result doesn't depend on scan order.
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (!parsed[i])
            continue;
        ComponentDefinitionStatus& status = out.status[i];
        const std::string& name = parsed[i]->name;
        if (isBuiltInComponentName(name)) {
            status.ok = false;
            status.error = std::format("{}: name \"{}\" is taken by a built-in component", status.path, name);
            continue;
        }
        std::string others;
        for (size_t j = 0; j < parsed.size(); ++j)
            if (j != i && out.status[j].name == name)
                others += std::format("{}{}", others.empty() ? "" : ", ", out.status[j].path);
        if (!others.empty()) {
            status.ok = false;
            status.error = std::format("{}: name \"{}\" is also declared in {}", status.path, name, others);
        }
    }
    for (size_t i = 0; i < parsed.size(); ++i)
        if (out.status[i].ok)
            out.valid.push_back(std::move(parsed[i]));
    return out;
}

bool activateComponentDefinitions(const std::optional<std::filesystem::path>& root, Scene* scene)
{
    const std::vector<std::shared_ptr<const LuaComponentDef>> previous = luaComponentTypes();
    ComponentDefinitions defs = root ? loadComponentDefinitions(*root) : ComponentDefinitions{};
    for (const ComponentDefinitionStatus& status : defs.status)
        if (!status.ok)
            log::error(std::format("Lua component rejected: {}", status.error));
    setLuaComponentTypes(std::move(defs.valid));
    ActiveState& state = active();
    state.root = root;
    state.files = root ? scan(*root) : Files{};
    state.status = std::move(defs.status);
    return scene && migrateLuaComponents(*scene, previous);
}

bool refreshComponentDefinitions(Scene& scene)
{
    ActiveState& state = active();
    if (!state.root || scan(*state.root) == state.files)
        return false;
    const std::optional<std::filesystem::path> root = state.root;
    const bool changed = activateComponentDefinitions(root, &scene);
    log::info(std::format("Reloaded Lua component definitions ({} OK, {} rejected)", luaComponentTypes().size(),
                          state.status.size() - luaComponentTypes().size()));
    return changed;
}

const std::vector<ComponentDefinitionStatus>& componentDefinitionStatus() { return active().status; }
const std::optional<std::filesystem::path>& activeComponentProject() { return active().root; }

bool migrateLuaComponents(Scene& scene, const std::vector<std::shared_ptr<const LuaComponentDef>>& previous)
{
    entt::registry& registry = scene.registry();
    bool changed = false;
    for (const auto& old : previous) {
        auto& pool = registry.storage<LuaComponentData>(luaStorageId(old->name));
        const ComponentType now = findAnyComponentType(old->name);
        const entt::sparse_set& set = pool; // iterates entities, not values
        const std::vector<entt::entity> entities(set.begin(), set.end());
        for (entt::entity entity : entities) {
            if (now.lua()) {
                std::vector<std::string> fixes;
                pool.get(entity).value = migrateLuaComponent(*now.lua(), pool.get(entity).value, fixes);
                warnFixes(registry, entity, old->name, fixes);
                changed = changed || !fixes.empty();
            } else {
                Json value = std::move(pool.get(entity).value);
                pool.remove(entity);
                registry.get_or_emplace<PreservedComponents>(entity).byName[old->name] = std::move(value);
                log::warn(std::format("{}: \"{}\" has no valid definition any more; its data is kept as-is",
                                      entityLabel(registry, entity), old->name));
            }
        }
    }
    const auto view = registry.view<PreservedComponents>();
    const std::vector<entt::entity> preserved(view.begin(), view.end());
    for (entt::entity entity : preserved) {
        const std::map<std::string, Json> byName = registry.get<PreservedComponents>(entity).byName;
        for (const auto& [name, value] : byName) {
            const ComponentType type = findAnyComponentType(name);
            if (!type.lua())
                continue;
            std::vector<std::string> fixes;
            Json converted = migrateLuaComponent(*type.lua(), value, fixes);
            removePreservedComponent(registry, entity, name);
            registry.storage<LuaComponentData>(luaStorageId(name)).emplace(entity, LuaComponentData{std::move(converted)});
            log::info(std::format("{}: \"{}\" has a definition again ({})", entityLabel(registry, entity), name,
                                  type.lua()->path));
            warnFixes(registry, entity, name, fixes);
            changed = changed || !fixes.empty();
        }
    }
    return changed;
}

} // namespace kast
