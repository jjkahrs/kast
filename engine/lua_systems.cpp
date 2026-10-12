#include "lua_systems.h"

#include "components.h"
#include "log.h"

#include <SDL3/SDL.h>
#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

// Base-library functions that load code from disk or reach outside a system's environment.
constexpr const char* kUnsafeGlobals[] = {"dofile", "loadfile", "load", "loadstring", "getfenv", "setfenv", "require", "module"};
constexpr const char* kQuaternionKeys[] = {"x", "y", "z", "w"};
constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53: Lua numbers are doubles

// Raises a Lua error at the calling script's file:line. sol2 turns the exception into lua_error.
[[noreturn]] void raise(lua_State* L, const std::string& message)
{
    luaL_where(L, 1);
    std::string where = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error(where + message);
}

std::string toString(lua_State* L, const sol::object& value)
{
    sol::protected_function tostring = sol::state_view(L)["tostring"];
    sol::protected_function_result result = tostring(value);
    return result.valid() && result.get_type() == sol::type::string ? result.get<std::string>() : "?";
}

std::string readFile(const std::filesystem::path& file, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = std::format("cannot open '{}'", file.string());
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool vectorType(const entt::meta_type& type, size_t& count)
{
    count = isType<bx::Vec3>(type) ? 3 : isType<bx::Quaternion>(type) ? 4 : 0;
    return count != 0;
}

struct System {
    std::string path; // as listed in the scene
    std::filesystem::path file;
    std::filesystem::file_time_type mtime{};
    sol::environment env;
    sol::protected_function update, shutdown;
    bool ok = false;
    std::string error;
};

} // namespace

struct LuaSystems::State {
    State(Scene& scene, const Input* input) : scene(scene), input(input) {}

    Scene& scene;
    const Input* input;
    sol::state lua;        // declared before everything holding Lua references, so it is destroyed last
    sol::table api;        // kast.* functions, copied into each system's own read-only kast table
    sol::table inputApi;   // kast.input.*
    std::vector<System> systems;
    std::unordered_map<EntityId, entt::entity> ids; // ponytail: rebuilt every tick, O(entities); index in Scene if big
    const System* current = nullptr; // whose code is running, for log prefixes

    void init(const std::filesystem::path& baseDir, const std::vector<std::string>& paths);
    void tick(float dt);
    void stop();

    void indexIds();
    void load(System& system);
    void reload(System& system);
    bool call(System& system, const sol::protected_function& fn, const char* what, std::optional<float> dt = {});
    void disable(System& system, const std::string& message);
    sol::table readOnly(const sol::table& source);
    sol::environment environment();

    // kast.* bindings
    entt::entity entity(lua_State* L, const sol::object& id);
    entt::meta_type componentType(lua_State* L, const std::string& name);
    Json fieldJson(lua_State* L, const entt::meta_type& type, const sol::object& value, const std::string& field);
    sol::object fieldLua(const entt::meta_type& type, const Json& value);
    void bindApi();
};

void LuaSystems::State::init(const std::filesystem::path& baseDir, const std::vector<std::string>& paths)
{
    // Only base, table, string and math: no io, os, ffi, debug, jit, bit or package.
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    for (const char* name : kUnsafeGlobals)
        lua.globals()[name] = sol::lua_nil;
    bindApi();
    indexIds();
    for (const std::string& path : paths)
        systems.push_back({.path = path, .file = baseDir / path});
    for (System& system : systems) {
        std::error_code ec;
        system.mtime = std::filesystem::last_write_time(system.file, ec);
        load(system);
    }
}

void LuaSystems::State::tick(float dt)
{
    indexIds();
    // ponytail: one stat per script per frame; poll on a timer if script counts grow large.
    for (System& system : systems) {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(system.file, ec);
        if (!ec && mtime != system.mtime) {
            system.mtime = mtime;
            reload(system);
        }
    }
    for (System& system : systems)
        if (system.ok)
            call(system, system.update, "update", dt);
}

void LuaSystems::State::stop()
{
    indexIds();
    for (System& system : systems)
        if (system.ok && system.shutdown)
            call(system, system.shutdown, "shutdown");
}

void LuaSystems::State::indexIds()
{
    ids.clear();
    for (auto [entity, id] : scene.registry().view<PersistentId>().each())
        ids.emplace(id.value, entity);
}

sol::table LuaSystems::State::readOnly(const sol::table& source)
{
    // A fresh copy behind a proxy: writes raise an error, and even rawset only changes this system's copy.
    sol::table copy = lua.create_table();
    for (const auto& [key, value] : source)
        copy[key] = value;
    sol::table meta = lua.create_table();
    meta["__index"] = copy;
    meta["__newindex"] = [](sol::this_state L, sol::object, sol::object, sol::object) { raise(L, "kast is read-only"); };
    meta["__metatable"] = false;
    sol::table proxy = lua.create_table();
    proxy[sol::metatable_key] = meta;
    return proxy;
}

sol::environment LuaSystems::State::environment()
{
    // Own copies of the globals and library tables, so one system's globals and edits never reach another.
    sol::environment env(lua, sol::create);
    for (const auto& [key, value] : lua.globals()) {
        if (value.get_type() != sol::type::table) {
            env[key] = value;
            continue;
        }
        sol::table copy = lua.create_table();
        for (const auto& [k, v] : value.as<sol::table>())
            copy[k] = v;
        env[key] = copy;
    }
    env["_G"] = env;
    sol::table inputTable = readOnly(inputApi);
    sol::table kast = lua.create_table();
    for (const auto& [key, value] : api)
        kast[key] = value;
    kast["input"] = inputTable;
    env["kast"] = readOnly(kast);
    return env;
}

void LuaSystems::State::load(System& system)
{
    system.ok = false;
    system.update = system.shutdown = sol::lua_nil;
    std::string error;
    const std::string code = readFile(system.file, error);
    if (!error.empty())
        return disable(system, std::format("{}: {}", system.path, error));
    // Text only: precompiled bytecode can break out of the sandbox.
    sol::load_result chunk = lua.load(code, "@" + system.path, sol::load_mode::text);
    if (!chunk.valid())
        return disable(system, chunk.get<sol::error>().what());
    system.env = environment();
    sol::protected_function body = chunk;
    sol::set_environment(system.env, body);

    current = &system;
    sol::protected_function_result result = body();
    current = nullptr;
    if (!result.valid())
        return disable(system, result.get<sol::error>().what());
    if (result.get_type() != sol::type::table)
        return disable(system, std::format("{}: must return a table with an update function", system.path));
    sol::table table = result;
    sol::object update = table["update"], init = table["init"], shutdown = table["shutdown"];
    if (update.get_type() != sol::type::function)
        return disable(system, std::format("{}: returned table has no update function", system.path));
    for (auto [name, fn] : {std::pair{"init", init}, std::pair{"shutdown", shutdown}})
        if (fn.get_type() != sol::type::lua_nil && fn.get_type() != sol::type::function)
            return disable(system, std::format("{}: \"{}\" must be a function", system.path, name));

    system.ok = true;
    system.error.clear();
    system.update = update;
    if (shutdown.get_type() == sol::type::function)
        system.shutdown = shutdown;
    if (init.get_type() == sol::type::function)
        call(system, init, "init");
}

void LuaSystems::State::reload(System& system)
{
    if (system.ok && system.shutdown)
        call(system, system.shutdown, "shutdown"); // an error here is logged; the reload goes ahead
    load(system);
    if (system.ok)
        log::info(std::format("Reloaded Lua system {}", system.path));
}

bool LuaSystems::State::call(System& system, const sol::protected_function& fn, const char* what, std::optional<float> dt)
{
    current = &system;
    std::string error;
    try {
        sol::protected_function_result result = dt ? fn(*dt) : fn();
        if (!result.valid())
            error = result.get<sol::error>().what();
    } catch (const std::exception& e) { // sol2 conversions; nothing may escape to the main loop
        error = std::format("{}: {}: {}", system.path, what, e.what());
    }
    current = nullptr;
    if (error.empty())
        return true;
    disable(system, error);
    return false;
}

void LuaSystems::State::disable(System& system, const std::string& message)
{
    system.ok = false;
    system.error = message;
    log::error(std::format("Lua system disabled: {}", message));
}

entt::entity LuaSystems::State::entity(lua_State* L, const sol::object& id)
{
    if (id.get_type() == sol::type::number) {
        const double value = id.as<double>();
        if (value >= 1.0 && value <= kMaxExactInteger && value == std::floor(value)) {
            const auto it = ids.find(static_cast<EntityId>(value));
            if (it != ids.end() && scene.valid(it->second) &&
                scene.registry().get<PersistentId>(it->second).value == it->first)
                return it->second;
        }
    }
    raise(L, std::format("invalid entity id {}", toString(L, id)));
}

entt::meta_type LuaSystems::State::componentType(lua_State* L, const std::string& name)
{
    const entt::meta_type type = findComponentType(name);
    if (!type)
        raise(L, std::format("unknown component \"{}\"", name));
    return type;
}

// A Lua field value as the JSON componentFromJson expects; it does the type and value checks.
Json LuaSystems::State::fieldJson(lua_State* L, const entt::meta_type& type, const sol::object& value, const std::string& field)
{
    size_t count = 0;
    if (vectorType(type, count)) {
        Json out = Json::array();
        if (value.get_type() == sol::type::table) {
            const sol::table table = value.as<sol::table>();
            for (size_t i = 0; i < count; ++i) {
                const sol::object c = table[kQuaternionKeys[i]];
                if (c.get_type() == sol::type::number && std::isfinite(c.as<double>()))
                    out.push_back(c.as<double>());
            }
        }
        if (out.size() != count)
            raise(L, std::format("\"{}\" must be a table {{x=, y=, z={}}} of finite numbers", field, count == 4 ? ", w=" : ""));
        return out;
    }
    switch (value.get_type()) {
    case sol::type::number: {
        const double number = value.as<double>();
        if (!std::isfinite(number))
            raise(L, std::format("\"{}\" must be a finite number", field));
        if (number == std::floor(number) && std::abs(number) <= kMaxExactInteger)
            return static_cast<int64_t>(number); // an integer, so int fields accept it
        return number;
    }
    case sol::type::boolean: return value.as<bool>();
    case sol::type::string: return value.as<std::string>();
    default: return nullptr; // rejected by componentFromJson with the field's expected type
    }
}

sol::object LuaSystems::State::fieldLua(const entt::meta_type& type, const Json& value)
{
    size_t count = 0;
    if (vectorType(type, count)) {
        sol::table table = lua.create_table();
        for (size_t i = 0; i < count; ++i)
            table[kQuaternionKeys[i]] = value[i].get<double>();
        return table;
    }
    if (value.is_boolean())
        return sol::make_object(lua, value.get<bool>());
    if (value.is_number())
        return sol::make_object(lua, value.get<double>());
    return sol::make_object(lua, value.get<std::string>()); // strings and enum names
}

void LuaSystems::State::bindApi()
{
    api = lua.create_table();
    entt::registry& registry = scene.registry();

    // Matches are captured when query is called: entities destroyed later in the loop are skipped, new ones are not
    // visited.
    api.set_function("query", [this, &registry](sol::this_state L, sol::variadic_args names) {
        std::vector<entt::meta_type> types;
        for (const sol::object name : names) {
            if (name.get_type() != sol::type::string)
                raise(L, std::format("component name must be a string, got {}", toString(L, name)));
            types.push_back(componentType(L, name.as<std::string>()));
        }
        std::vector<EntityId> matches;
        scene.forEachDepthFirst([&](entt::entity e) {
            if (std::ranges::all_of(types, [&](const entt::meta_type& type) { return hasComponent(registry, e, type); }))
                matches.push_back(registry.get<PersistentId>(e).value);
        });
        return std::function<sol::optional<EntityId>()>([this, matches = std::move(matches), next = size_t{0}]() mutable {
            while (next < matches.size()) {
                const EntityId id = matches[next++];
                const auto it = ids.find(id);
                if (it != ids.end() && scene.valid(it->second) && scene.registry().get<PersistentId>(it->second).value == id)
                    return sol::optional<EntityId>(id);
            }
            return sol::optional<EntityId>();
        });
    });
    api.set_function("has", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name) {
        return hasComponent(registry, entity(L, id), componentType(L, name));
    });
    api.set_function("add", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name) {
        const entt::entity e = entity(L, id);
        const entt::meta_type type = componentType(L, name);
        if (!hasComponent(registry, e, type))
            addComponent(registry, e, type);
    });
    api.set_function("remove", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name) {
        const entt::entity e = entity(L, id);
        const entt::meta_type type = componentType(L, name);
        if (!componentInfo(type).removable)
            raise(L, std::format("\"{}\" cannot be removed", name));
        if (hasComponent(registry, e, type))
            removeComponent(registry, e, type);
    });
    api.set_function("get", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name) {
        const entt::entity e = entity(L, id);
        const entt::meta_type type = componentType(L, name);
        if (!hasComponent(registry, e, type))
            raise(L, std::format("entity {} has no \"{}\"", toString(L, id), name));
        const Json json = componentToJson(getComponent(registry, e, type));
        sol::table table = lua.create_table();
        for (auto [fieldId, field] : type.data())
            table[field.name()] = fieldLua(field.type(), json[field.name()]);
        return table;
    });
    // Partial tables: listed fields change, the rest keep their values. All-or-nothing: any bad field or a value
    // ComponentInfo::validate rejects leaves the component unchanged.
    // ponytail: goes through JSON to reuse the scene-file checks; convert meta_any directly if it shows in profiles.
    api.set_function("set", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name,
                                              const sol::table& fields) {
        const entt::entity e = entity(L, id);
        const entt::meta_type type = componentType(L, name);
        if (!hasComponent(registry, e, type))
            raise(L, std::format("entity {} has no \"{}\"", toString(L, id), name));
        Json json = componentToJson(getComponent(registry, e, type));
        for (const auto& [key, value] : fields) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("\"{}\": field names must be strings, got {}", name, toString(L, key)));
            const std::string field = key.as<std::string>();
            const entt::meta_data data = type.data(entt::hashed_string{field.c_str(), field.size()});
            if (!data)
                raise(L, std::format("\"{}\": unknown field \"{}\"", name, field));
            json[field] = fieldJson(L, data.type(), value, field);
        }
        std::string error;
        const entt::meta_any component = componentFromJson(type, json, error);
        if (!component)
            raise(L, std::format("\"{}\": {}", name, error));
        setComponent(registry, e, component);
    });
    api.set_function("create", [this, &registry](sol::this_state L, const std::string& name, const sol::object& parent) {
        const bool root = parent.get_type() == sol::type::lua_nil || parent.get_type() == sol::type::none;
        const entt::entity e = scene.create(name, root ? entt::null : entity(L, parent));
        const EntityId id = registry.get<PersistentId>(e).value;
        ids[id] = e;
        return id;
    });
    api.set_function("destroy", [this](sol::this_state L, const sol::object& id) { scene.destroy(entity(L, id)); });

    const auto logger = [this](void (*write)(std::string_view)) {
        return [this, write](sol::this_state L, const sol::object& message) {
            write(std::format("{}: {}", current ? current->path : "lua", toString(L, message)));
        };
    };
    api.set_function("log", logger(log::info));
    api.set_function("warn", logger(log::warn));
    api.set_function("error", logger(log::error));

    inputApi = lua.create_table();
    inputApi.set_function("keyDown", [this](sol::this_state L, const std::string& name) {
        const SDL_Scancode scancode = SDL_GetScancodeFromName(name.c_str());
        if (scancode == SDL_SCANCODE_UNKNOWN)
            raise(L, std::format("unknown key name \"{}\"", name));
        return input && input->keysDown[scancode];
    });
    inputApi.set_function("mouseDelta", [this] {
        return input ? std::make_tuple(input->mouseDeltaX, input->mouseDeltaY) : std::make_tuple(0.0f, 0.0f);
    });
}

LuaSystems::LuaSystems() = default;
LuaSystems::~LuaSystems() = default;

void LuaSystems::start(Scene& scene, const std::filesystem::path& baseDir, const Input* input)
{
    state_ = std::make_unique<State>(scene, input);
    state_->init(baseDir, scene.systems);
}

void LuaSystems::tick(float dt)
{
    if (state_)
        state_->tick(dt);
}

void LuaSystems::stop()
{
    if (!state_)
        return;
    state_->stop();
    state_.reset();
}

std::vector<LuaSystemStatus> LuaSystems::status() const
{
    std::vector<LuaSystemStatus> out;
    if (state_)
        for (const System& system : state_->systems)
            out.push_back({system.path, system.ok, system.error});
    return out;
}

} // namespace kast
