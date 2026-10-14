#include "lua_systems.h"

#include "components.h"
#include "log.h"
#include "lua_sandbox.h"
#include "lua_state_machines.h"
#include "material.h"
#include "project.h"
#include "scene_io.h"

#include <SDL3/SDL.h>
#include <sol/sol.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53: Lua numbers are doubles

// Raises a Lua error at the calling script's file:line. sol2 turns the exception into lua_error.
[[noreturn]] void raise(lua_State* L, const std::string& message)
{
    luaL_where(L, 1);
    std::string where = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error(where + message);
}

struct System {
    std::string path; // as listed in the scene
    std::filesystem::path file;
    std::filesystem::file_time_type mtime{};
    sol::environment env;
    sol::protected_function update, shutdown;
    std::vector<EventBus::Subscription> subscriptions; // kast.subscribe; cleared on load and disable
    bool ok = false;
    std::string error;
};

// A kast.setMaterialProperty value as JSON for setMaterialField: numbers and strings as they are, sequences as arrays,
// {x, y, z[, w]} tables as [x, y, z(, w)]. Anything else becomes null, which every field rejects.
Json materialValueToJson(const sol::object& value)
{
    switch (value.get_type()) {
    case sol::type::number:
        return value.as<double>();
    case sol::type::string:
        return value.as<std::string>();
    case sol::type::table: {
        const sol::table table = value.as<sol::table>();
        Json array = Json::array();
        const bool vector = table["x"].valid();
        for (int i = 1; i <= (vector ? 4 : 5); ++i) { // a 5th element makes it too long for any field
            const sol::object element = vector ? sol::object(table[std::string(1, "xyzw"[i - 1])]) : sol::object(table[i]);
            if (element.get_type() == sol::type::lua_nil)
                break;
            array.push_back(element.get_type() == sol::type::number ? Json(element.as<double>()) : Json());
        }
        return array;
    }
    default:
        return nullptr;
    }
}

} // namespace

// A scene switch requested by kast.loadScene, applied at the end of the frame.
struct PendingScene {
    std::string path; // project-relative
    Scene scene;
};

struct LuaSystems::State {
    State(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, std::string scenePath, const Input* input,
          EventBus& bus, StateMachines& fsm)
        : scene(scene), projectRoot(projectRoot), scenePath(std::move(scenePath)), input(input), bus(bus), fsm(fsm)
    {
    }

    Scene& scene;
    std::optional<std::filesystem::path> projectRoot;
    std::string scenePath; // project-relative, empty if unsaved or no project
    const Input* input;
    EventBus& bus;
    StateMachines& fsm;
    MaterialLibrary materials; // shared .mat files, for kast.get/setMaterialProperty
    std::optional<PendingScene> pending; // last kast.loadScene call wins
    bool inShutdown = false;             // kast.loadScene is ignored while a shutdown runs
    sol::state lua;        // declared before everything holding Lua references, so it is destroyed last
    sol::table api;        // kast.* functions, copied into each system's own read-only kast table
    sol::table inputApi;   // kast.input.*
    sol::table fsmApi;     // kast.fsm.*
    DefinitionFiles fsmFiles; // state machine definition files at the last load, for hot reload
    std::string callbackPath; // the definition file whose enter/exit is running (log prefixes, event publisher)
    std::vector<System> systems;
    std::unordered_map<EntityId, entt::entity> ids; // ponytail: rebuilt every tick, O(entities); index in Scene if big
    System* current = nullptr; // whose code is running: log prefixes, event publisher and subscription owner

    void init(const std::vector<std::string>& paths);
    void tick(float dt);
    void stop();

    void indexIds();
    entt::entity find(EntityId id);
    std::string who() const; // the running code's file, for log prefixes and event publishers
    void loadMachines(bool reload);
    StateCallback bindCallback(const sol::protected_function& fn, const std::string& path, const std::string& what);
    void load(System& system);
    void reload(System& system);
    template <class... Args>
    bool call(System& system, const sol::protected_function& fn, std::string_view what, Args&&... args);
    void disable(System& system, const std::string& message);
    sol::table readOnly(const sol::table& source);
    sol::environment environment();

    // kast.* bindings
    entt::entity entity(lua_State* L, const sol::object& id);
    ComponentType componentType(lua_State* L, const std::string& name);
    const Material& material(lua_State* L, entt::entity entity); // its instance, else its MeshRenderer's material
    std::string eventName(lua_State* L, const char* function, const sol::object& name);
    void bindApi();
    void bindFsmApi();
};

void LuaSystems::State::init(const std::vector<std::string>& paths)
{
    openSandbox(lua);
    bindApi();
    indexIds();
    fsm.attach(scene, [this](EntityId id) { return find(id); });
    if (projectRoot)
        loadMachines(false);
    for (const std::string& path : paths)
        systems.push_back({.path = path});
    for (System& system : systems) {
        std::string error = "no project (open a scene inside a project to run Lua systems)";
        const std::optional<std::filesystem::path> file =
            projectRoot ? resolveProjectPath(*projectRoot, system.path, error) : std::nullopt;
        if (!file) {
            disable(system, std::format("{}: {}", system.path, error));
            continue;
        }
        system.file = *file;
        std::error_code ec;
        system.mtime = std::filesystem::last_write_time(system.file, ec);
        load(system);
    }
    indexIds(); // entities the inits created
    fsm.enterAll();
}

void LuaSystems::State::tick(float dt)
{
    indexIds();
    // ponytail: one stat per script per frame; poll on a timer if script counts grow large.
    for (System& system : systems) {
        if (system.file.empty()) // never resolved: no project or a path outside it
            continue;
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(system.file, ec);
        if (!ec && mtime != system.mtime) {
            system.mtime = mtime;
            reload(system);
        }
    }
    // ponytail: lists fsms/ every frame; poll on a timer if definition counts grow large.
    if (projectRoot && scanStateMachineFiles(*projectRoot) != fsmFiles)
        loadMachines(true);
    fsm.syncAll(); // entities that gained StateMachine (or changed its machine) outside kast.add/kast.set
    for (System& system : systems)
        if (system.ok)
            call(system, system.update, "update", dt);
    bus.drain();
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

entt::entity LuaSystems::State::find(EntityId id)
{
    const auto it = ids.find(id);
    if (it != ids.end() && scene.valid(it->second) && scene.registry().get<PersistentId>(it->second).value == id)
        return it->second;
    for (auto [entity, persistent] : scene.registry().view<PersistentId>().each()) // created since the last index
        if (persistent.value == id)
            return ids[id] = entity;
    return entt::null;
}

std::string LuaSystems::State::who() const
{
    return current ? current->path : callbackPath.empty() ? std::string("lua") : callbackPath;
}

// Every definition file is reloaded (a fresh environment each). Valid definitions replace the running ones; a broken
// or deleted file keeps its last valid definition.
void LuaSystems::State::loadMachines(bool reload)
{
    fsmFiles = scanStateMachineFiles(*projectRoot);
    StateMachineDefinitions defs = parseStateMachineDefinitions(
        lua, *projectRoot, [this] { return environment(); },
        [this](const sol::protected_function& fn, const std::string& path, const std::string& what) {
            return bindCallback(fn, path, what);
        });
    for (const ComponentDefinitionStatus& status : defs.status)
        if (!status.ok)
            log::error(std::format("State machine rejected: {}", status.error));
    const size_t ok = defs.valid.size();
    for (StateMachineDef& def : defs.valid)
        fsm.define(std::move(def));
    if (reload)
        log::info(std::format("Reloaded state machine definitions ({} OK, {} rejected)", ok, defs.status.size() - ok));
}

StateCallback LuaSystems::State::bindCallback(const sol::protected_function& fn, const std::string& path,
                                              const std::string& what)
{
    return [this, callback = fn, path, what](const Transition& t) { // errors come back with a traceback, as for systems
        sol::table ctx = lua.create_table();
        ctx["machine"] = t.machine;
        if (t.entity)
            ctx["entity"] = *t.entity;
        if (t.from)
            ctx["from"] = *t.from;
        ctx["to"] = t.to;
        System* caller = std::exchange(current, nullptr); // a system's kast.fsm.change may have called this
        std::string callerPath = std::exchange(callbackPath, path);
        std::string error;
        try {
            sol::protected_function_result result = callback(ctx);
            if (!result.valid())
                error = result.get<sol::error>().what();
        } catch (const std::exception& e) { // sol2 conversions; nothing may escape to the main loop
            error = std::format("{}: {}", path, e.what());
        }
        current = caller;
        callbackPath = std::move(callerPath);
        if (!error.empty())
            log::error(std::format("State machine {}: {} failed: {}", path, what, error));
    };
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
    sol::environment env = sandboxEnvironment(lua);
    sol::table inputTable = readOnly(inputApi);
    sol::table kast = lua.create_table();
    for (const auto& [key, value] : api)
        kast[key] = value;
    kast["input"] = inputTable;
    kast["fsm"] = readOnly(fsmApi);
    env["kast"] = readOnly(kast);
    return env;
}

void LuaSystems::State::load(System& system)
{
    system.ok = false;
    system.update = system.shutdown = sol::lua_nil;
    system.subscriptions.clear(); // the old instance's
    std::string error;
    system.env = environment();
    sol::protected_function body = loadSandboxed(lua, system.file, "@" + system.path, system.env, error);
    if (!body.valid())
        return disable(system, error.starts_with("cannot open") ? std::format("{}: {}", system.path, error) : error);

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

template <class... Args>
bool LuaSystems::State::call(System& system, const sol::protected_function& fn, std::string_view what, Args&&... args)
{
    current = &system;
    inShutdown = what == "shutdown";
    std::string error;
    try {
        sol::protected_function_result result = fn(std::forward<Args>(args)...);
        if (!result.valid())
            error = result.get<sol::error>().what();
    } catch (const std::exception& e) { // sol2 conversions; nothing may escape to the main loop
        error = std::format("{}: {}: {}", system.path, what, e.what());
    }
    current = nullptr;
    inShutdown = false;
    if (error.empty())
        return true;
    disable(system, error);
    return false;
}

void LuaSystems::State::disable(System& system, const std::string& message)
{
    system.ok = false;
    system.error = message;
    system.subscriptions.clear(); // until a successful hot reload
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

ComponentType LuaSystems::State::componentType(lua_State* L, const std::string& name)
{
    const ComponentType type = findAnyComponentType(name);
    if (!type)
        raise(L, std::format("unknown component \"{}\"", name));
    return type;
}

const Material& LuaSystems::State::material(lua_State* L, entt::entity e)
{
    if (const auto* instance = scene.registry().try_get<MaterialInstance>(e))
        return instance->material;
    const auto* mesh = scene.registry().try_get<MeshRenderer>(e);
    if (!mesh)
        raise(L, std::format("entity {} has no \"MeshRenderer\"", scene.registry().get<PersistentId>(e).value));
    materials.setRoot(projectRoot.value_or(std::filesystem::path()));
    materials.reloadChanged();
    return materials.get(mesh->material);
}

std::string LuaSystems::State::eventName(lua_State* L, const char* function, const sol::object& name)
{
    if (name.get_type() != sol::type::string || name.as<std::string>().empty())
        raise(L, std::format("{}: event name must be a non-empty string, got {}", function, toString(L, name)));
    return name.as<std::string>();
}

void LuaSystems::State::bindApi()
{
    api = lua.create_table();
    entt::registry& registry = scene.registry();

    // Matches are captured when query is called: entities destroyed later in the loop are skipped, new ones are not
    // visited.
    api.set_function("query", [this, &registry](sol::this_state L, sol::variadic_args names) {
        std::vector<ComponentType> types;
        for (const sol::object name : names) {
            if (name.get_type() != sol::type::string)
                raise(L, std::format("component name must be a string, got {}", toString(L, name)));
            types.push_back(componentType(L, name.as<std::string>()));
        }
        std::vector<EntityId> matches;
        scene.forEachDepthFirst([&](entt::entity e) {
            if (std::ranges::all_of(types, [&](const ComponentType& type) { return hasComponent(registry, e, type); }))
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
        const ComponentType type = componentType(L, name);
        if (!hasComponent(registry, e, type))
            addComponent(registry, e, type);
        fsm.syncEntity(e); // a new StateMachine enters its initial state
    });
    api.set_function("remove", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name) {
        const entt::entity e = entity(L, id);
        const ComponentType type = componentType(L, name);
        if (!type.removable())
            raise(L, std::format("\"{}\" cannot be removed", name));
        if (hasComponent(registry, e, type))
            removeComponent(registry, e, type);
    });
    api.set_function("get", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name) {
        const entt::entity e = entity(L, id);
        const ComponentType type = componentType(L, name);
        if (!hasComponent(registry, e, type))
            raise(L, std::format("entity {} has no \"{}\"", toString(L, id), name));
        const Json json = componentToJson(registry, e, type);
        sol::table table = lua.create_table();
        for (const FieldDesc& field : type.fields())
            table[field.name] = fieldJsonToLua(lua, field, json[field.name]);
        return table;
    });
    // Partial tables: listed fields change, the rest keep their values. All-or-nothing: any bad field or a value
    // ComponentInfo::validate (built-ins) or min/max / enum values (Lua components) reject leaves it unchanged.
    // ponytail: goes through JSON to reuse the scene-file checks; convert meta_any directly if it shows in profiles.
    api.set_function("set", [this, &registry](sol::this_state L, const sol::object& id, const std::string& name,
                                              const sol::table& fields) {
        const entt::entity e = entity(L, id);
        const ComponentType type = componentType(L, name);
        if (!hasComponent(registry, e, type))
            raise(L, std::format("entity {} has no \"{}\"", toString(L, id), name));
        Json json = componentToJson(registry, e, type);
        std::string error;
        for (const auto& [key, value] : fields) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("\"{}\": field names must be strings, got {}", name, toString(L, key)));
            const std::string field = key.as<std::string>();
            const auto& descs = type.fields();
            const auto desc = std::ranges::find(descs, field, &FieldDesc::name);
            if (desc == descs.end())
                raise(L, std::format("\"{}\": unknown field \"{}\"", name, field));
            if (!luaToFieldJson(*desc, value, json[field], error))
                raise(L, error);
        }
        const auto* mesh = registry.try_get<MeshRenderer>(e);
        const std::string oldMaterial = mesh ? mesh->material : "";
        if (!setComponentJson(registry, e, type, json, error))
            raise(L, std::format("\"{}\": {}", name, error));
        if (mesh && registry.get<MeshRenderer>(e).material != oldMaterial)
            registry.remove<MaterialInstance>(e); // a material swap drops the entity's instance of the old one
        fsm.syncEntity(e); // a changed StateMachine "machine" enters the new machine's initial state
    });
    api.set_function("create", [this, &registry](sol::this_state L, const std::string& name, const sol::object& parent) {
        const bool root = parent.get_type() == sol::type::lua_nil || parent.get_type() == sol::type::none;
        const entt::entity e = scene.create(name, root ? entt::null : entity(L, parent));
        const EntityId id = registry.get<PersistentId>(e).value;
        ids[id] = e;
        return id;
    });
    api.set_function("destroy", [this](sol::this_state L, const sol::object& id) { scene.destroy(entity(L, id)); });

    // Per-entity material properties (Unity's renderer.material): the first set copies the entity's MeshRenderer
    // material into a MaterialInstance only it renders with; reads come from the instance, else the shared .mat.
    // Fields and checks as in .mat files; colors as {r, g, b[, a]} or {x =, y =, z =[, w =]}.
    api.set_function("getMaterialProperty", [this](sol::this_state L, const sol::object& id, const std::string& name) {
        const entt::entity e = entity(L, id);
        const std::optional<Json> value = materialField(material(L, e), name);
        if (!value)
            raise(L, std::format("unknown material property \"{}\"", name));
        if (!value->is_array())
            return sol::make_object(lua, value->is_string() ? sol::make_object(lua, value->get<std::string>())
                                                            : sol::make_object(lua, value->get<double>()));
        sol::table table = lua.create_table();
        for (size_t i = 0; i < value->size(); ++i)
            table[i + 1] = (*value)[i].get<double>();
        return sol::make_object(lua, table);
    });
    api.set_function("setMaterialProperty", [this, &registry](sol::this_state L, const sol::object& id,
                                                              const std::string& name, const sol::object& value) {
        const entt::entity e = entity(L, id);
        Material copy = material(L, e);
        std::string error;
        if (!setMaterialField(copy, name, materialValueToJson(value), error))
            raise(L, error);
        registry.emplace_or_replace<MaterialInstance>(e, std::move(copy));
    });

    const auto logger = [this](void (*write)(std::string_view)) {
        return [this, write](sol::this_state L, const sol::object& message) {
            write(std::format("{}: {}", who(), toString(L, message)));
        };
    };
    api.set_function("log", logger(log::info));
    api.set_function("warn", logger(log::warn));
    api.set_function("error", logger(log::error));

    // Validated now (raises at the calling line); applied by LuaSystems::tick after every update has run.
    api.set_function("loadScene", [this](sol::this_state L, const std::string& path) {
        if (inShutdown)
            return; // the session is ending
        if (!projectRoot)
            raise(L, "kast.loadScene: no project is active");
        std::string error;
        const std::optional<std::filesystem::path> file = resolveProjectPath(*projectRoot, path, error);
        if (!file)
            raise(L, std::format("kast.loadScene: {}", error));
        Scene loaded;
        if (!loadScene(file->string(), loaded, error))
            raise(L, std::format("kast.loadScene: {}", error));
        pending = PendingScene{toProjectPath(*projectRoot, *file).value_or(path), std::move(loaded)};
    });
    api.set_function("currentScene", [this]() -> sol::optional<std::string> {
        return scenePath.empty() ? sol::nullopt : sol::optional<std::string>(scenePath);
    });

    // Events: queued, delivered by bus.drain() after every update (see README "Events"). Subscriptions belong to the
    // calling system; a handle is a light userdata holding the subscription id (no metatable a script could edit).
    api.set_function("publish", [this](sol::this_state L, const sol::object& name, const sol::object& payload) {
        std::string event = eventName(L, "kast.publish", name);
        nlohmann::json json;
        if (std::string error; !luaToJson(payload, json, error))
            raise(L, std::format("kast.publish: {}", error));
        bus.publish(std::move(event), std::move(json), who());
    });
    api.set_function("subscribe", [this](sol::this_state L, const sol::object& name, const sol::object& fn) {
        std::string event = eventName(L, "kast.subscribe", name);
        if (fn.get_type() != sol::type::function)
            raise(L, std::format("kast.subscribe: callback must be a function, got {}", toString(L, fn)));
        if (!current)
            raise(L, "kast.subscribe: only a system can subscribe");
        System* owner = current;
        std::string what = std::format("event '{}'", event);
        EventBus::Subscription subscription = bus.subscribe(
            std::move(event), [this, owner, what = std::move(what), callback = sol::protected_function(fn)](
                                  const nlohmann::json& payload) {
                if (owner->ok) // a disabled system's subscriptions are gone; this guards the rest of its drain
                    call(*owner, callback, what, jsonToLua(lua, payload));
            });
        const uint64_t id = subscription.id();
        owner->subscriptions.push_back(std::move(subscription));
        return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); // ponytail: 64-bit ids in a pointer, x64 only
    });
    api.set_function("unsubscribe", [this](sol::this_state L, const sol::object& handle) {
        if (handle.get_type() != sol::type::lightuserdata)
            raise(L, std::format("kast.unsubscribe: expected a handle from kast.subscribe, got {}", toString(L, handle)));
        const uint64_t id = reinterpret_cast<uintptr_t>(handle.as<void*>());
        // Another system's handle, or one already unsubscribed, isn't in current's list: nothing happens.
        if (current)
            std::erase_if(current->subscriptions, [id](const EventBus::Subscription& s) { return s.id() == id; });
    });

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
    bindFsmApi();
}

// kast.fsm.*: thin wrappers over StateMachines, whose std::invalid_argument (a bad target) becomes a Lua error at the
// calling line. See README "State machines".
void LuaSystems::State::bindFsmApi()
{
    using Target = StateMachines::Target;
    const auto target = [](lua_State* L, const char* function, const sol::object& value) -> Target {
        if (value.get_type() == sol::type::string)
            return value.as<std::string>();
        if (value.get_type() == sol::type::number) {
            const double id = value.as<double>();
            if (id >= 1.0 && id <= kMaxExactInteger && id == std::floor(id))
                return static_cast<EntityId>(id);
            raise(L, std::format("{}: invalid entity id {}", function, toString(L, value)));
        }
        raise(L, std::format("{}: target must be a state machine name or an entity id, got {}", function,
                             toString(L, value)));
    };
    const auto stateName = [](lua_State* L, const char* function, const sol::object& value) {
        if (value.get_type() != sol::type::string)
            raise(L, std::format("{}: state must be a string, got {}", function, toString(L, value)));
        return value.as<std::string>();
    };
    // Runs body; a bad target raises at the calling line.
    const auto guarded = [](lua_State* L, const char* function, auto&& body) {
        try {
            return body();
        } catch (const std::invalid_argument& e) {
            raise(L, std::format("{}: {}", function, e.what()));
        }
    };

    fsmApi = lua.create_table();
    fsmApi.set_function("state", [this, target, guarded](sol::this_state L, const sol::object& t) {
        const Target machine = target(L, "kast.fsm.state", t);
        return guarded(L, "kast.fsm.state", [&] { return fsm.state(machine); });
    });
    fsmApi.set_function("change", [this, target, stateName, guarded](sol::this_state L, const sol::object& t,
                                                                     const sol::object& state) {
        const Target machine = target(L, "kast.fsm.change", t);
        const std::string to = stateName(L, "kast.fsm.change", state);
        return guarded(L, "kast.fsm.change", [&] { return fsm.change(machine, to); });
    });
    fsmApi.set_function("canChange", [this, target, stateName, guarded](sol::this_state L, const sol::object& t,
                                                                        const sol::object& state) {
        const Target machine = target(L, "kast.fsm.canChange", t);
        const std::string to = stateName(L, "kast.fsm.canChange", state);
        return guarded(L, "kast.fsm.canChange", [&] { return fsm.canChange(machine, to); });
    });
    fsmApi.set_function("states", [this, target, guarded](sol::this_state L, const sol::object& t) {
        const Target machine = target(L, "kast.fsm.states", t);
        return sol::as_table(guarded(L, "kast.fsm.states", [&] { return fsm.states(machine); }));
    });
}

LuaSystems::LuaSystems() = default;
LuaSystems::~LuaSystems() = default;

void LuaSystems::start(Scene& scene, const std::optional<std::filesystem::path>& projectRoot,
                       const std::string& scenePath, const Input* input, std::vector<std::string> gameMachines)
{
    fsm_.reset(std::move(gameMachines)); // its Lua callbacks go before the Lua state
    state_.reset(); // its subscriptions go first
    bus_.reset();
    launch(scene, projectRoot, scenePath, input);
}

void LuaSystems::launch(Scene& scene, const std::optional<std::filesystem::path>& projectRoot,
                        const std::string& scenePath, const Input* input)
{
    fsm_.detach(); // drops callbacks into the old Lua state
    state_.reset(); // the old Lua state goes before the new one is built
    state_ = std::make_unique<State>(scene, projectRoot, scenePath, input, bus_, fsm_);
    state_->init(scene.systems);
}

bool LuaSystems::tick(float dt)
{
    if (!state_)
        return false;
    state_->tick(dt);
    if (!state_->pending)
        return false;
    PendingScene next = std::move(*state_->pending);
    Scene& scene = state_->scene;
    const std::optional<std::filesystem::path> projectRoot = state_->projectRoot;
    const Input* input = state_->input;
    endSession(); // old systems' shutdowns see the old scene; the Lua state and queued events are discarded
    scene = std::move(next.scene);
    log::info(std::format("Switched to scene '{}'", next.path));
    launch(scene, projectRoot, next.path, input); // C++ subscriptions, the bus history and game states carry over
    return true;
}

void LuaSystems::endSession()
{
    state_->stop();
    fsm_.detach();
    state_.reset();
    bus_.dropQueued();
}

void LuaSystems::stop()
{
    if (!state_)
        return;
    endSession();
    fsm_.reset();
}

std::vector<LuaSystemStatus> LuaSystems::status() const
{
    std::vector<LuaSystemStatus> out;
    if (state_)
        for (const System& system : state_->systems)
            out.push_back({system.path, system.ok, system.error});
    return out;
}

std::string LuaSystems::scenePath() const { return state_ ? state_->scenePath : std::string(); }

} // namespace kast
