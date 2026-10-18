#include "lua_systems.h"

#include "animation.h"
#include "components.h"
#include "log.h"
#include "lua_sandbox.h"
#include "lua_state_machines.h"
#include "lua_world_api.h"
#include "files.h"
#include "material.h"
#include "math3d.h"
#include "network.h"
#include "project.h"
#include "scene_io.h"

#include <SDL3/SDL.h>
#include <sol/sol.hpp>

#include <algorithm>
#include <array>
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

// A kast.setMaterialProperty value as JSON for setMaterialField: numbers, booleans and strings as they are, sequences as arrays,
// {x, y, z[, w]} tables as [x, y, z(, w)]. Anything else becomes null, which every field rejects.
Json materialValueToJson(const sol::object& value)
{
    switch (value.get_type()) {
    case sol::type::number:
        return value.as<double>();
    case sol::type::boolean:
        return value.as<bool>();
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
    State(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, std::string scenePath, Input* input,
          EventBus& bus, StateMachines& fsm, Physics& physics, Audio& audio, Network& network, AuthClient& auth,
          WorldClient& world, GuiAccess* gui)
        : scene(scene), projectRoot(projectRoot), scenePath(std::move(scenePath)), input(input), bus(bus), fsm(fsm),
          physics(physics), audio(audio), network(network), auth(auth), world(world), gui(gui)
    {
    }

    Scene& scene;
    std::optional<std::filesystem::path> projectRoot;
    std::string scenePath; // project-relative, empty if unsaved or no project
    Input* input;
    EventBus& bus;
    StateMachines& fsm;
    Physics& physics;
    Audio& audio;
    Network& network;
    AuthClient& auth;
    WorldClient& world;
    GuiAccess* gui; // null: no game GUIs here, kast.gui is nil
    std::unordered_map<entt::entity, AudioHandle> sources; // the last voice each AudioSource started (kast.audio.stopSource)
    MaterialLibrary materials; // shared .mat files, for kast.get/setMaterialProperty
    std::optional<PendingScene> pending; // last kast.loadScene call wins
    bool inShutdown = false;             // kast.loadScene is ignored while a shutdown runs
    sol::state lua;        // declared before everything holding Lua references, so it is destroyed last
    sol::table api;        // kast.* functions, copied into each system's own read-only kast table
    sol::table inputApi;   // kast.input.*
    sol::table fsmApi;     // kast.fsm.*
    sol::table physicsApi; // kast.physics.*
    sol::table audioApi;   // kast.audio.*
    sol::table netApi;     // kast.net.*
    sol::table authApi;    // kast.auth.*, only while Online Services are enabled
    sol::table worldApi;   // kast.world.*, likewise
    sol::table guiApi;     // kast.gui.*, only with a GuiAccess
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
    void bindPhysicsApi();
    void bindAudioApi();
    void bindNetApi();
    void bindAuthApi();
    void bindWorldApi();
    void bindGuiApi();
    // Starts entity's AudioSource. Throws std::invalid_argument (see Audio::play).
    AudioHandle playSource(entt::entity entity);
    void playOnStart(); // every AudioSource with playOnStart; a bad clip logs an error and plays nothing
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
            const ScriptBudget budget(lua.lua_state());
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
    kast["physics"] = readOnly(physicsApi);
    kast["audio"] = readOnly(audioApi);
    kast["net"] = readOnly(netApi);
    if (auth.enabled())
        kast["auth"] = readOnly(authApi);
    if (world.enabled())
        kast["world"] = readOnly(worldApi);
    if (gui)
        kast["gui"] = readOnly(guiApi);
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
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state());
        return body();
    }();
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
        const ScriptBudget budget(lua.lua_state());
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
        if (e == entt::null)
            raise(L, std::format("parent is already {} levels deep (the hierarchy limit)", kMaxHierarchyDepth));
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
        if (value->is_boolean())
            return sol::make_object(lua, value->get<bool>());
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

    // Skeletal animation (see animation.h) on an entity with an Animator and a ModelRenderer. playAnimation also writes
    // the Animator's clip / loop / speed, so kast.get shows what plays.
    const auto animated = [this, &registry](lua_State* L, const sol::object& id) {
        const entt::entity e = entity(L, id);
        const auto* renderer = registry.try_get<ModelRenderer>(e);
        if (!renderer || !registry.all_of<Animator>(e))
            raise(L, std::format("entity {} needs an \"Animator\" and a \"ModelRenderer\"", toString(L, id)));
        std::shared_ptr<const Model> model = models().get(renderer->model);
        if (!model)
            raise(L, std::format("entity {}: model '{}' is not loaded", toString(L, id), renderer->model));
        return std::pair{e, std::move(model)};
    };
    api.set_function("playAnimation", [this, &registry, animated](sol::this_state L, const sol::object& id,
                                                                  const std::string& clip, sol::optional<sol::table> opts) {
        const auto [e, model] = animated(L, id);
        const int index = findClip(*model, clip);
        if (index < 0)
            raise(L, std::format("model '{}' has no clip \"{}\"", registry.get<ModelRenderer>(e).model, clip));
        Animator& animator = registry.get<Animator>(e);
        bool loop = animator.loop;
        float speed = animator.speed, fade = 0.0f;
        if (opts) {
            loop = opts->get_or("loop", loop);
            speed = opts->get_or("speed", speed);
            fade = opts->get_or("fade", fade);
        }
        if (!(speed >= 0.0f && speed <= kMaxAnimatorSpeed))
            raise(L, std::format("playAnimation: speed must be between 0 and {}, got {}", kMaxAnimatorSpeed, speed));
        if (!(fade >= 0.0f))
            raise(L, std::format("playAnimation: fade must be >= 0 seconds, got {}", fade));
        animator.clip = clip;
        animator.loop = loop;
        animator.speed = speed;
        AnimatorState& state = registry.get_or_emplace<AnimatorState>(e);
        if (state.model != model.get()) { // not posed yet: as animatorSystem's first tick
            state.model = model.get();
            state.pose.clear();
        }
        state.started = true;
        state.clipName = clip;
        state.playing = true;
        playClip(state, index, fade);
    });
    // Stops at the current pose.
    api.set_function("stopAnimation", [this, &registry, animated](sol::this_state L, const sol::object& id) {
        const entt::entity e = animated(L, id).first;
        AnimatorState& state = registry.get_or_emplace<AnimatorState>(e);
        if (!state.started) { // never started: stay in bind pose instead of running playOnStart later
            state.started = true;
            state.clipName = registry.get<Animator>(e).clip;
            state.clip = -1;
        }
        state.playing = false;
    });
    // {clip = name, time = seconds into the clip, playing = bool}
    api.set_function("getAnimationState", [this, &registry, animated](sol::this_state L, const sol::object& id) {
        const auto [e, model] = animated(L, id);
        const Animator& animator = registry.get<Animator>(e);
        const auto* state = registry.try_get<AnimatorState>(e);
        float time = 0.0f;
        if (state && state->clip >= 0 && static_cast<size_t>(state->clip) < model->clips.size())
            time = clipTime(state->time, model->clips[static_cast<size_t>(state->clip)].duration, animator.loop);
        sol::table table = lua.create_table();
        table["clip"] = animator.clip;
        table["time"] = time;
        table["playing"] = state && state->playing;
        return table;
    });
    api.set_function("getAnimationClips", [this, animated](sol::this_state L, const sol::object& id) {
        const std::shared_ptr<const Model> model = animated(L, id).second;
        sol::table table = lua.create_table();
        for (size_t i = 0; i < model->clips.size(); ++i)
            table[i + 1] = model->clips[i].name;
        return table;
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

    // Events: queued, delivered by bus.drain() after every update (see docs/engine/events.html). Subscriptions belong to the
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
            std::move(event), [this, owner, what = std::move(what),
                               // on the main thread: fn's own may be a coroutine, collected before the drain
                               callback = sol::protected_function(lua.lua_state(), fn)](const nlohmann::json& payload) {
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
    inputApi.set_function("mouseDown", [this](sol::this_state L, const sol::object& name) {
        static constexpr std::array<std::string_view, Input::kButtonCount> names{"left", "middle", "right", "x1", "x2"};
        const auto found = name.get_type() == sol::type::string
                               ? std::ranges::find(names, name.as<std::string_view>())
                               : names.end();
        if (found == names.end())
            raise(L, std::format("kast.input.mouseDown: unknown button {} (left, middle, right, x1 or x2)",
                                 toString(L, name)));
        return input && input->buttonsDown[static_cast<size_t>(found - names.begin())];
    });
    // kast.input.mouseRay(): origin, direction ({x, y, z} each, direction normalized) of the ray from the active
    // Camera through the cursor, for kast.physics.raycast; nil before the cursor's position is known or with no Camera.
    inputApi.set_function("mouseRay", [this](sol::this_state L) -> std::tuple<sol::object, sol::object> {
        const entt::entity camera = activeCamera(scene);
        if (!input || input->mouseX < 0.0f || input->view.height <= 0.0f || camera == entt::null)
            return {sol::lua_nil, sol::lua_nil};
        float world[16];
        scene.worldMatrix(camera, world);
        const float tanHalf = std::tan(bx::toRad(scene.registry().get<Camera>(camera).verticalFovDegrees) * 0.5f);
        const float aspect = input->view.width / input->view.height;
        const float ndcX = input->mouseX * 2.0f - 1.0f;
        const float ndcY = 1.0f - input->mouseY * 2.0f;
        const bx::Vec3 local = {ndcX * tanHalf * aspect, ndcY * tanHalf, -1.0f}; // the camera looks down -z
        const bx::Vec3 dir = bx::normalize(transformDir(local, world));
        const auto table = [&](const bx::Vec3& v) {
            return sol::make_object(L, lua.create_table_with("x", v.x, "y", v.y, "z", v.z));
        };
        return {table({world[12], world[13], world[14]}), table(dir)};
    });
    inputApi.set_function("wheelDelta", [this] { return input ? input->wheelDelta : 0.0f; });
    inputApi.set_function("setCursorLocked", [this](sol::this_state L, const sol::object& locked) {
        if (locked.get_type() != sol::type::boolean)
            raise(L, std::format("kast.input.setCursorLocked: expected a boolean, got {}", toString(L, locked)));
        if (input)
            input->cursorLocked = locked.as<bool>();
    });
    bindFsmApi();
    bindPhysicsApi();
    bindAudioApi();
    bindNetApi();
    bindAuthApi();
    bindWorldApi();
    bindGuiApi();
}

// kast.fsm.*: thin wrappers over StateMachines, whose std::invalid_argument (a bad target) becomes a Lua error at the
// calling line. See docs/engine/state-machines.html.
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

// kast.physics.*: wrappers over Physics, whose std::invalid_argument (no body of the right kind, a bad size) becomes a
// Lua error at the calling line. Vectors are {x =, y =, z =} or {x, y, z}; rotations {x =, y =, z =, w =} or
// {x, y, z, w}. See docs/engine/physics.html.
void LuaSystems::State::bindPhysicsApi()
{
    const auto number = [](lua_State* L, const std::string& function, const char* what, const sol::object& value) {
        if (value.get_type() != sol::type::number || !std::isfinite(value.as<double>()))
            raise(L, std::format("{}: {} must be a finite number, got {}", function, what, toString(L, value)));
        return value.as<float>();
    };
    const auto components = [number](lua_State* L, const std::string& function, const char* what,
                                     const sol::object& value, int count) {
        if (value.get_type() != sol::type::table)
            raise(L, std::format("{}: {} must be a table {{x, y, z{}}}, got {}", function, what, count == 4 ? ", w" : "",
                                 toString(L, value)));
        const sol::table table = value.as<sol::table>();
        const bool named = table["x"].valid();
        std::array<float, 4> out{};
        for (int i = 0; i < count; ++i) {
            const sol::object element = named ? sol::object(table[std::string(1, "xyzw"[i])]) : sol::object(table[i + 1]);
            out[i] = number(L, function, what, element);
        }
        return out;
    };
    const auto vec3 = [components](lua_State* L, const std::string& function, const char* what, const sol::object& value) {
        const auto c = components(L, function, what, value, 3);
        return bx::Vec3{c[0], c[1], c[2]};
    };
    const auto toLua = [this](const bx::Vec3& v) {
        sol::table table = lua.create_table();
        table["x"] = v.x;
        table["y"] = v.y;
        table["z"] = v.z;
        return table;
    };
    const auto idOf = [this](entt::entity e) { return scene.registry().get<PersistentId>(e).value; };
    const auto idList = [this, idOf](const std::vector<entt::entity>& entities) {
        sol::table table = lua.create_table();
        for (size_t i = 0; i < entities.size(); ++i)
            table[i + 1] = idOf(entities[i]);
        return table;
    };
    // Runs body; a Physics std::invalid_argument raises at the calling line.
    const auto guarded = [](lua_State* L, const std::string& function, auto&& body) {
        try {
            return body();
        } catch (const std::invalid_argument& e) {
            raise(L, std::format("{}: {}", function, e.what()));
        }
    };

    physicsApi = lua.create_table();
    physicsApi.set_function("raycast", [this, vec3, number, toLua, idOf, guarded](
                                           sol::this_state L, const sol::object& origin, const sol::object& direction,
                                           const sol::object& maxDistance) -> sol::object {
        const std::string fn = "kast.physics.raycast";
        const bx::Vec3 from = vec3(L, fn, "origin", origin);
        const bx::Vec3 dir = vec3(L, fn, "direction", direction);
        const float distance = number(L, fn, "maxDistance", maxDistance);
        const auto hit = guarded(L, fn, [&] { return physics.raycast(from, dir, distance); });
        if (!hit)
            return sol::lua_nil;
        sol::table table = lua.create_table();
        table["entity"] = idOf(hit->entity);
        table["point"] = toLua(hit->point);
        table["normal"] = toLua(hit->normal);
        table["distance"] = hit->distance;
        return table;
    });
    physicsApi.set_function("overlapSphere", [this, vec3, number, idList, guarded](
                                                 sol::this_state L, const sol::object& center, const sol::object& radius) {
        const std::string fn = "kast.physics.overlapSphere";
        const bx::Vec3 c = vec3(L, fn, "center", center);
        const float r = number(L, fn, "radius", radius);
        return idList(guarded(L, fn, [&] { return physics.overlapSphere(c, r); }));
    });
    physicsApi.set_function("overlapBox", [this, vec3, components, idList, guarded](
                                              sol::this_state L, const sol::object& center, const sol::object& halfExtents,
                                              const sol::object& rotation) {
        const std::string fn = "kast.physics.overlapBox";
        const bx::Vec3 c = vec3(L, fn, "center", center);
        const bx::Vec3 half = vec3(L, fn, "halfExtents", halfExtents);
        bx::Quaternion q{bx::InitIdentity};
        if (rotation.get_type() != sol::type::lua_nil && rotation.get_type() != sol::type::none) {
            const auto r = components(L, fn, "rotation", rotation, 4);
            q = {r[0], r[1], r[2], r[3]};
            if (!(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3] > 1e-12f))
                raise(L, std::format("{}: rotation must not be zero", fn));
        }
        return idList(guarded(L, fn, [&] { return physics.overlapBox(c, half, q); }));
    });

    // kast.physics.<name>(id) -> {x, y, z}
    const auto getter = [&](const char* name, bx::Vec3 (Physics::*get)(entt::entity) const) {
        physicsApi.set_function(name, [this, fn = std::format("kast.physics.{}", name), get, toLua,
                                       guarded](sol::this_state L, const sol::object& id) {
            const entt::entity e = entity(L, id);
            return toLua(guarded(L, fn, [&] { return (physics.*get)(e); }));
        });
    };
    // kast.physics.<name>(id, {x, y, z})
    const auto setter = [&](const char* name, void (Physics::*set)(entt::entity, const bx::Vec3&)) {
        physicsApi.set_function(name, [this, fn = std::format("kast.physics.{}", name), set, vec3,
                                       guarded](sol::this_state L, const sol::object& id, const sol::object& value) {
            const entt::entity e = entity(L, id);
            const bx::Vec3 v = vec3(L, fn, "value", value);
            guarded(L, fn, [&] { (physics.*set)(e, v); });
        });
    };
    getter("getVelocity", &Physics::velocity);
    setter("setVelocity", &Physics::setVelocity);
    getter("getAngularVelocity", &Physics::angularVelocity);
    setter("setAngularVelocity", &Physics::setAngularVelocity);
    setter("addForce", &Physics::addForce);
    setter("addImpulse", &Physics::addImpulse);
    setter("moveCharacter", &Physics::moveCharacter);
    physicsApi.set_function("isGrounded", [this, guarded](sol::this_state L, const sol::object& id) {
        const entt::entity e = entity(L, id);
        return guarded(L, "kast.physics.isGrounded", [&] { return physics.isGrounded(e); });
    });
    physicsApi.set_function("getGround", [this, toLua, idOf, guarded](sol::this_state L,
                                                                      const sol::object& id) -> sol::object {
        const entt::entity e = entity(L, id);
        const auto ground = guarded(L, "kast.physics.getGround", [&] { return physics.ground(e); });
        if (!ground)
            return sol::lua_nil;
        sol::table table = lua.create_table();
        table["entity"] = idOf(ground->entity);
        table["normal"] = toLua(ground->normal);
        return table;
    });
}

AudioHandle LuaSystems::State::playSource(entt::entity entity)
{
    const AudioSource& source = scene.registry().get<AudioSource>(entity);
    if (source.clip.empty())
        throw std::invalid_argument("the AudioSource has no clip");
    const AudioHandle handle = audio.play(source.clip, {source.bus, source.volume, source.loop});
    sources[entity] = handle;
    return handle;
}

void LuaSystems::State::playOnStart()
{
    scene.forEachDepthFirst([&](entt::entity e) {
        const AudioSource* source = scene.registry().try_get<AudioSource>(e);
        if (!source || !source->playOnStart)
            return;
        try {
            playSource(e);
        } catch (const std::invalid_argument& error) {
            log::error(std::format("AudioSource on '{}': {}", scene.registry().get<Name>(e).value, error.what()));
        }
    });
}

void LuaSystems::State::bindAudioApi()
{
    const auto busArg = [](lua_State* L, const std::string& function, const sol::object& name) {
        const std::optional<AudioBus> found =
            name.get_type() == sol::type::string ? busFromName(name.as<std::string>()) : std::nullopt;
        if (!found)
            raise(L, std::format("{}: unknown bus {} (expected \"Master\", \"Music\", \"SFX\", \"UI\" or \"Voice\")",
                                 function, toString(L, name)));
        return *found;
    };
    const auto volume = [](lua_State* L, const std::string& function, const sol::object& value) {
        if (value.get_type() != sol::type::number || !(value.as<double>() >= 0.0 && value.as<double>() <= 1.0))
            raise(L, std::format("{}: volume must be a number between 0 and 1, got {}", function, toString(L, value)));
        return value.as<float>();
    };
    const auto boolean = [](lua_State* L, const std::string& function, const char* what, const sol::object& value) {
        if (value.get_type() != sol::type::boolean)
            raise(L, std::format("{}: {} must be true or false, got {}", function, what, toString(L, value)));
        return value.as<bool>();
    };
    const auto handle = [](lua_State* L, const std::string& function, const sol::object& value) {
        const double h = value.get_type() == sol::type::number ? value.as<double>() : -1.0;
        if (!(h >= 0.0 && h <= kMaxExactInteger && h == std::floor(h)))
            raise(L, std::format("{}: invalid sound handle {}", function, toString(L, value)));
        return static_cast<AudioHandle>(h);
    };
    // Runs body; an Audio std::invalid_argument raises at the calling line.
    const auto guarded = [](lua_State* L, const std::string& function, auto&& body) {
        try {
            return body();
        } catch (const std::invalid_argument& e) {
            raise(L, std::format("{}: {}", function, e.what()));
        }
    };
    const auto source = [this](lua_State* L, const std::string& function, const sol::object& id) {
        const entt::entity e = entity(L, id);
        if (!scene.registry().all_of<AudioSource>(e))
            raise(L, std::format("{}: entity {} has no AudioSource", function, toString(L, id)));
        return e;
    };
    const auto absent = [](const sol::object& value) {
        return value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none;
    };

    audioApi = lua.create_table();
    audioApi.set_function("play", [this, busArg, volume, boolean, guarded, absent](
                                      sol::this_state L, const sol::object& path, const sol::object& opts) {
        const std::string fn = "kast.audio.play";
        if (path.get_type() != sol::type::string)
            raise(L, std::format("{}: path must be a string, got {}", fn, toString(L, path)));
        Audio::PlayOptions options;
        if (opts.get_type() == sol::type::table) {
            for (const auto& [key, value] : opts.as<sol::table>()) {
                const std::string name = key.get_type() == sol::type::string ? key.as<std::string>() : "";
                if (name == "bus")
                    options.bus = busArg(L, fn, value);
                else if (name == "volume")
                    options.volume = volume(L, fn, value);
                else if (name == "loop")
                    options.loop = boolean(L, fn, "loop", value);
                else
                    raise(L, std::format("{}: unknown option {} (expected bus, volume or loop)", fn, toString(L, key)));
            }
        } else if (!absent(opts)) {
            raise(L, std::format("{}: options must be a table, got {}", fn, toString(L, opts)));
        }
        return static_cast<double>(guarded(L, fn, [&] { return audio.play(path.as<std::string>(), options); }));
    });
    // kast.audio.<name>(handle): a stale handle is ignored.
    const auto onHandle = [&](const char* name, void (Audio::*fn)(AudioHandle)) {
        audioApi.set_function(name, [this, function = std::format("kast.audio.{}", name), fn,
                                     handle](sol::this_state L, const sol::object& h) { (audio.*fn)(handle(L, function, h)); });
    };
    onHandle("stop", &Audio::stop);
    onHandle("pause", &Audio::pause);
    onHandle("resume", &Audio::resume);
    audioApi.set_function("setVolume", [this, handle, volume](sol::this_state L, const sol::object& h,
                                                              const sol::object& v) {
        audio.setVolume(handle(L, "kast.audio.setVolume", h), volume(L, "kast.audio.setVolume", v));
    });
    audioApi.set_function("isPlaying", [this, handle](sol::this_state L, const sol::object& h) {
        return audio.isPlaying(handle(L, "kast.audio.isPlaying", h));
    });
    audioApi.set_function("stopAll", [this, busArg, absent](sol::this_state L, const sol::object& name) {
        if (absent(name))
            audio.stopAll();
        else
            audio.stopAll(busArg(L, "kast.audio.stopAll", name));
    });
    audioApi.set_function("setBusVolume", [this, busArg, volume](sol::this_state L, const sol::object& name,
                                                              const sol::object& v) {
        audio.setBusVolume(busArg(L, "kast.audio.setBusVolume", name), volume(L, "kast.audio.setBusVolume", v));
    });
    audioApi.set_function("getBusVolume", [this, busArg](sol::this_state L, const sol::object& name) {
        return audio.busVolume(busArg(L, "kast.audio.getBusVolume", name));
    });
    audioApi.set_function("setBusMuted", [this, busArg, boolean](sol::this_state L, const sol::object& name,
                                                              const sol::object& muted) {
        const std::string fn = "kast.audio.setBusMuted";
        audio.setBusMuted(busArg(L, fn, name), boolean(L, fn, "muted", muted));
    });
    audioApi.set_function("playSource", [this, source, guarded](sol::this_state L, const sol::object& id) {
        const std::string fn = "kast.audio.playSource";
        const entt::entity e = source(L, fn, id);
        return static_cast<double>(guarded(L, fn, [&] { return playSource(e); }));
    });
    audioApi.set_function("stopSource", [this, source](sol::this_state L, const sol::object& id) {
        const entt::entity e = source(L, "kast.audio.stopSource", id);
        if (const auto it = sources.find(e); it != sources.end())
            audio.stop(it->second);
    });
}

// kast.auth.*: thin wrappers over AuthClient; arguments must be strings (raises otherwise), results are events.
void LuaSystems::State::bindAuthApi()
{
    const auto text = [](lua_State* L, const char* function, const char* what, const sol::object& value) {
        if (value.get_type() != sol::type::string)
            raise(L, std::format("kast.auth.{}: {} must be a string, got {}", function, what, toString(L, value)));
        return value.as<std::string>();
    };
    const auto id = [](uint64_t id) { return static_cast<double>(id); };
    authApi = lua.create_table();
    authApi.set_function("register", [=, this](sol::this_state L, const sol::object& email, const sol::object& password) {
        return id(auth.registerAccount(text(L, "register", "email", email), text(L, "register", "password", password)));
    });
    authApi.set_function("resendVerification", [=, this](sol::this_state L, const sol::object& email) {
        return id(auth.resendVerification(text(L, "resendVerification", "email", email)));
    });
    authApi.set_function("verifyEmail", [=, this](sol::this_state L, const sol::object& token) {
        return id(auth.verifyEmail(text(L, "verifyEmail", "token", token)));
    });
    authApi.set_function("login", [=, this](sol::this_state L, const sol::object& email, const sol::object& password) {
        return id(auth.login(text(L, "login", "email", email), text(L, "login", "password", password)));
    });
    authApi.set_function("logout", [=, this]() { return id(auth.logout()); });
    authApi.set_function("forgotPassword", [=, this](sol::this_state L, const sol::object& email) {
        return id(auth.forgotPassword(text(L, "forgotPassword", "email", email)));
    });
    authApi.set_function("resetPassword", [=, this](sol::this_state L, const sol::object& token,
                                                    const sol::object& password) {
        return id(auth.resetPassword(text(L, "resetPassword", "token", token),
                                     text(L, "resetPassword", "password", password)));
    });
    authApi.set_function("me", [=, this]() { return id(auth.me()); });
    authApi.set_function("realms", [=, this]() { return id(auth.realms()); });
    authApi.set_function("isLoggedIn", [this]() { return auth.loggedIn(); });
    // {accountId, email, role} or nil; never the tokens.
    authApi.set_function("account", [this]() -> sol::object {
        return auth.loggedIn() ? jsonToLua(lua, auth.account()) : sol::object(sol::lua_nil);
    });
}

// kast.gui.*: the scene's game GUIs through GuiAccess (docs/engine/game-gui.html). Values and texts are never logged: a text
// field may hold a password.
void LuaSystems::State::bindGuiApi()
{
    const auto text = [](lua_State* L, const char* function, const char* what, const sol::object& value) {
        if (value.get_type() != sol::type::string)
            raise(L, std::format("kast.gui.{}: {} must be a string, got {}", function, what, toString(L, value)));
        return value.as<std::string>();
    };
    guiApi = lua.create_table();
    guiApi.set_function("value", [=, this](sol::this_state L, const sol::object& id) -> sol::object {
        const std::optional<std::string> value = gui->value(text(L, "value", "id", id));
        return value ? sol::make_object(lua, *value) : sol::object(sol::lua_nil);
    });
    guiApi.set_function("setText", [=, this](sol::this_state L, const sol::object& id, const sol::object& value) {
        gui->setText(text(L, "setText", "id", id), text(L, "setText", "text", value));
    });
    guiApi.set_function("setVisible", [=, this](sol::this_state L, const sol::object& id, const sol::object& visible) {
        if (visible.get_type() != sol::type::boolean)
            raise(L, std::format("kast.gui.setVisible: visible must be a boolean, got {}", toString(L, visible)));
        gui->setVisible(text(L, "setVisible", "id", id), visible.as<bool>());
    });
    guiApi.set_function("focusedField", [this]() -> sol::object {
        const std::optional<std::string> id = gui->focusedField();
        return id ? sol::make_object(lua, *id) : sol::object(sol::lua_nil);
    });
    guiApi.set_function("mouseOver", [this] { return gui->mouseOver(); });
}

// kast.world.*: see lua_world_api.h.
void LuaSystems::State::bindWorldApi()
{
    worldApi = makeWorldApi(lua, world);
}

void LuaSystems::State::bindNetApi()
{
    const auto absent = [](const sol::object& value) {
        return value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none;
    };
    const auto string = [](lua_State* L, const std::string& function, const char* what, const sol::object& value) {
        if (value.get_type() != sol::type::string)
            raise(L, std::format("{}: {} must be a string, got {}", function, what, toString(L, value)));
        return value.as<std::string>();
    };
    // Raises unless value is one of names; returns its index.
    const auto choice = [string](lua_State* L, const std::string& function, const char* what, const sol::object& value,
                                 std::initializer_list<const char*> names) {
        const std::string text = string(L, function, what, value);
        int i = 0;
        std::string expected;
        for (const char* name : names) {
            if (text == name)
                return i;
            expected += std::format("{}'{}'", i++ ? ", " : "", name);
        }
        raise(L, std::format("{}: {} must be one of {}, got '{}'", function, what, expected, text));
    };
    const auto timeout = [](lua_State* L, const std::string& function, const sol::object& value) {
        const double seconds = value.get_type() == sol::type::number ? value.as<double>() : -1.0;
        if (!(seconds > 0.0 && seconds <= 86400.0))
            raise(L, std::format("{}: timeout must be a number of seconds in (0, 86400], got {}", function,
                                 toString(L, value)));
        return std::chrono::milliseconds(std::max<int64_t>(1, std::llround(seconds * 1000.0)));
    };
    const auto caPem = [this, string](lua_State* L, const std::string& function, const sol::object& value) {
        const std::string path = string(L, function, "caFile", value);
        if (!projectRoot)
            raise(L, std::format("{}: caFile needs an active project", function));
        std::string error, pem;
        const std::optional<std::filesystem::path> file = resolveProjectPath(*projectRoot, path, error);
        if (!file)
            raise(L, std::format("{}: caFile: {}", function, error));
        if (!readFile(*file, pem))
            raise(L, std::format("{}: caFile '{}' can't be read", function, path));
        return pem;
    };
    // Lua data as the bytes to send: the reverse of what a receive under encoding gives.
    const auto outgoing = [string](lua_State* L, const std::string& function, NetEncoding encoding,
                                   const sol::object& data) {
        if (encoding == NetEncoding::Json) {
            nlohmann::json json;
            if (std::string error; !luaToJson(data, json, error))
                raise(L, std::format("{}: {}", function, error));
            return json.dump();
        }
        std::string text = string(L, function, "data", data);
        if (encoding == NetEncoding::Base64) {
            std::optional<std::string> bytes = base64Decode(text);
            if (!bytes)
                raise(L, std::format("{}: data is not valid base64 (encoding 'base64')", function));
            return std::move(*bytes);
        }
        if (!isUtf8(text))
            raise(L, std::format("{}: data is not valid UTF-8 (encoding 'text'; use 'base64' for bytes)", function));
        return text;
    };
    // Passes each key of options (a table) to set(key, value); unknown keys raise.
    const auto forEachOption = [](lua_State* L, const std::string& function, const sol::object& options,
                                  std::initializer_list<const char*> allowed, auto&& set) {
        if (options.get_type() != sol::type::table)
            raise(L, std::format("{}: expected an options table, got {}", function, toString(L, options)));
        for (const auto& [key, value] : options.as<sol::table>()) {
            const std::string name = key.get_type() == sol::type::string ? key.as<std::string>() : "";
            if (std::ranges::none_of(allowed, [&](const char* a) { return name == a; }))
                raise(L, std::format("{}: unknown option {}", function, toString(L, key)));
            set(name, value);
        }
    };
    // Runs body; a Network std::invalid_argument raises at the calling line.
    const auto guarded = [](lua_State* L, const std::string& function, auto&& body) {
        try {
            return body();
        } catch (const std::invalid_argument& e) {
            raise(L, std::format("{}: {}", function, e.what()));
        }
    };

    const auto send = [this, outgoing, guarded](lua_State* L, const std::string& conn, const sol::object& data) {
        const std::string fn = "kast.net.send";
        const std::optional<NetEncoding> encoding = network.encoding(conn);
        if (!encoding)
            raise(L, std::format("{}: no connection named '{}'", fn, conn));
        std::string bytes = outgoing(L, fn, *encoding, data);
        guarded(L, fn, [&] { network.send(conn, std::move(bytes)); });
    };
    // A read-only {name, send, close, state} table. The methods take the handle first (h:send(data)) and act by name,
    // so a handle from an earlier scene acts on whatever connection has that name now.
    const auto handle = [this, send](const std::string& name) {
        sol::table h = lua.create_table();
        h["name"] = name;
        h.set_function("send", [send, name](sol::this_state L, const sol::object&, const sol::object& data) {
            send(L, name, data);
        });
        h.set_function("close", [this, name](const sol::object&) { network.close(name); });
        h.set_function("state", [this, name](const sol::object&) {
            return std::string(netStateName(network.state(name).value_or(NetState::Closed)));
        });
        return readOnly(h);
    };

    netApi = lua.create_table();
    netApi.set_function("open", [=, this](sol::this_state L, const sol::object& opts) {
        const std::string fn = "kast.net.open";
        NetOpenOptions options;
        std::vector<std::string> given;
        forEachOption(L, fn, opts,
                      {"name", "type", "host", "port", "url", "tls", "caFile", "encoding", "framing", "timeout"},
                      [&](const std::string& key, const sol::object& value) {
                          given.push_back(key);
                          if (key == "name") {
                              options.name = string(L, fn, "name", value);
                          } else if (key == "type") {
                              options.type = static_cast<NetType>(choice(L, fn, "type", value, {"tcp", "ws"}));
                          } else if (key == "host") {
                              options.host = string(L, fn, "host", value);
                          } else if (key == "port") {
                              const double port = value.get_type() == sol::type::number ? value.as<double>() : 0.0;
                              if (!(port >= 1.0 && port <= 65535.0 && port == std::floor(port)))
                                  raise(L, std::format("{}: port must be an integer 1..65535, got {}", fn,
                                                       toString(L, value)));
                              options.port = static_cast<uint16_t>(port);
                          } else if (key == "url") {
                              options.url = string(L, fn, "url", value);
                          } else if (key == "tls") {
                              if (value.get_type() != sol::type::boolean)
                                  raise(L, std::format("{}: tls must be true or false, got {}", fn, toString(L, value)));
                              options.tls = value.as<bool>();
                          } else if (key == "caFile") {
                              options.caPem = caPem(L, fn, value);
                          } else if (key == "encoding") {
                              options.encoding =
                                  static_cast<NetEncoding>(choice(L, fn, "encoding", value, {"text", "base64", "json"}));
                          } else if (key == "framing") {
                              options.framing =
                                  static_cast<NetFraming>(choice(L, fn, "framing", value, {"raw", "u16le", "line"}));
                          } else {
                              options.timeout = timeout(L, fn, value);
                          }
                      });
        const auto has = [&](const char* key) { return std::ranges::find(given, key) != given.end(); };
        if (!has("type"))
            raise(L, std::format("{}: type must be 'tcp' or 'ws'", fn));
        const bool tcp = options.type == NetType::Tcp;
        for (const char* key : {"url", "host", "port", "tls", "framing"})
            if (has(key) && tcp == (std::string_view(key) == "url"))
                raise(L, std::format("{}: {} doesn't apply to type '{}'", fn, key, tcp ? "tcp" : "ws"));
        const std::string name = options.name;
        guarded(L, fn, [&] { network.open(std::move(options)); });
        return handle(name);
    });
    netApi.set_function("get", [this, string, handle](sol::this_state L, const sol::object& name) -> sol::object {
        const std::string conn = string(L, "kast.net.get", "name", name);
        if (!network.has(conn))
            return sol::lua_nil;
        return handle(conn);
    });
    netApi.set_function("close", [this, string](sol::this_state L, const sol::object& name) {
        network.close(string(L, "kast.net.close", "name", name));
    });
    netApi.set_function("request", [=, this](sol::this_state L, const sol::object& opts) {
        const std::string fn = "kast.net.request";
        NetRequestOptions options;
        std::optional<sol::object> body;
        forEachOption(L, fn, opts, {"method", "url", "headers", "body", "timeout", "encoding", "caFile"},
                      [&](const std::string& key, const sol::object& value) {
                          if (key == "method") {
                              options.method = string(L, fn, "method", value);
                          } else if (key == "url") {
                              options.url = string(L, fn, "url", value);
                          } else if (key == "headers") {
                              if (value.get_type() != sol::type::table)
                                  raise(L, std::format("{}: headers must be a table of strings, got {}", fn,
                                                       toString(L, value)));
                              for (const auto& [k, v] : value.as<sol::table>()) {
                                  if (k.get_type() != sol::type::string || v.get_type() != sol::type::string)
                                      raise(L, std::format("{}: headers must map strings to strings, got {} = {}", fn,
                                                           toString(L, k), toString(L, v)));
                                  options.headers.emplace_back(k.as<std::string>(), v.as<std::string>());
                              }
                          } else if (key == "body") {
                              body = value;
                          } else if (key == "timeout") {
                              options.timeout = timeout(L, fn, value);
                          } else if (key == "encoding") {
                              options.encoding =
                                  static_cast<NetEncoding>(choice(L, fn, "encoding", value, {"text", "base64", "json"}));
                          } else {
                              options.caPem = caPem(L, fn, value);
                          }
                      });
        if (body && !absent(*body))
            options.body = outgoing(L, fn, options.encoding, *body);
        return static_cast<double>(guarded(L, fn, [&] { return network.request(std::move(options)); }));
    });
    netApi.set_function("base64Encode", [string](sol::this_state L, const sol::object& bytes) {
        return base64Encode(string(L, "kast.net.base64Encode", "bytes", bytes));
    });
    netApi.set_function("base64Decode", [string](sol::this_state L, const sol::object& text) {
        std::optional<std::string> bytes = base64Decode(string(L, "kast.net.base64Decode", "text", text));
        if (!bytes)
            raise(L, "kast.net.base64Decode: not valid base64");
        return std::move(*bytes);
    });
}


LuaSystems::LuaSystems() = default;
LuaSystems::~LuaSystems() = default;

void LuaSystems::start(Scene& scene, const std::optional<std::filesystem::path>& projectRoot,
                       const std::string& scenePath, Input* input, std::vector<std::string> gameMachines,
                       std::optional<AuthConfig> auth)
{
    fsm_.reset(std::move(gameMachines)); // its Lua callbacks go before the Lua state
    state_.reset(); // its subscriptions go first
    bus_.reset();
    network_.stop();
    replication_.stop(); // forgets the remote players
    world_.reset(auth);
    auth_.reset(std::move(auth));
    audio_.start(projectRoot);
    terrain_.start(projectRoot);
    launch(scene, projectRoot, scenePath, input);
}

void LuaSystems::launch(Scene& scene, const std::optional<std::filesystem::path>& projectRoot,
                        const std::string& scenePath, Input* input)
{
    fsm_.detach(); // drops callbacks into the old Lua state
    state_.reset(); // the old Lua state goes before the new one is built
    models().setRoot(projectRoot.value_or(std::filesystem::path())); // kast.*Animation* load models before any render
    replication_.start(scene, bus_, world_); // takes the template out before physics and scripts see it
    physics_.start(scene, bus_); // before the inits, which may query it
    if (gui_)
        gui_->reset(); // the old scene's documents and writes; the inits' writes apply as the new documents load
    state_ = std::make_unique<State>(scene, projectRoot, scenePath, input, bus_, fsm_, physics_, audio_, network_, auth_,
                                     world_, gui_);
    state_->playOnStart(); // before the inits, which may stop them
    state_->init(scene.systems);
}

bool LuaSystems::tick(float dt)
{
    if (!state_)
        return false;
    audio_.update();
    // Network results wait in network_ (not the bus queue a scene switch drops), so a switch's new systems, whose
    // inits ran at the end of the previous tick, get everything received since.
    // The auth and world clients' own results answer only to them: they hold tokens and raw frames.
    for (NetEvent& event : network_.poll())
        if (!auth_.handle(event) && !world_.handle(event))
            bus_.publish(std::move(event.name), std::move(event.payload), "kast.net");
    for (NetEvent& event : auth_.poll()) {
        world_.observe(event); // realm lists, for kast.world.connect
        bus_.publish(std::move(event.name), std::move(event.payload), "kast.auth");
    }
    for (NetEvent& event : world_.poll())
        bus_.publish(std::move(event.name), std::move(event.payload), "kast.world");
    state_->tick(dt);
    replication_.update(dt); // sends what this frame's systems did to the local player
    if (!state_->pending)
        return false;
    PendingScene next = std::move(*state_->pending);
    Scene& scene = state_->scene;
    const std::optional<std::filesystem::path> projectRoot = state_->projectRoot;
    Input* input = state_->input;
    if (input)
        input->clear(); // held buttons and the cursor lock belong to the old scene
    endSession(); // old systems' shutdowns see the old scene; the Lua state and queued events are discarded
    scene = std::move(next.scene);
    log::info(std::format("Switched to scene '{}'", next.path));
    launch(scene, projectRoot, next.path, input); // C++ subscriptions, the bus history and game states carry over
    return true;
}

void LuaSystems::endSession()
{
    state_->stop();
    replication_.stop(true); // the scene goes; the remote players stay in view on the connection
    fsm_.detach();
    state_.reset();
    physics_.stop();
    audio_.stop();
    bus_.dropQueued();
}

void LuaSystems::stop()
{
    if (!state_)
        return;
    endSession();
    fsm_.reset();
    replication_.stop();
    network_.stop();
    world_.reset();
    auth_.reset(); // memory only: Stop logs out
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
