#include "lua_systems.h"

#include "animation.h"
#include "components.h"
#include "log.h"
#include "lua_items.h"
#include "lua_sandbox.h"
#include "lua_state_machines.h"
#include "lua_chat_api.h"
#include "lua_world_api.h"
#include "files.h"
#include "material.h"
#include "math3d.h"
#include "network.h"
#include "particles.h"
#include "project.h"
#include "scene_io.h"
#include "shader.h"

#include <SDL3/SDL.h>
#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <format>
#include <functional>
#include <list>
#include <map>
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

// A kast.loadScene / unloadScene / setActiveScene call, applied at the end of the frame in call order.
struct SceneRequest {
    enum class Kind { Switch, Add, Unload, Activate } kind = Kind::Switch;
    std::string path; // project-relative
    Scene scene;      // Switch, Add: read and checked at the call
};

struct LuaSystems::State {
    State(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, const std::string& scenePath, Input* input,
          EventBus& bus, StateMachines& fsm, Physics& physics, Audio& audio, Network& network, AuthClient& auth,
          WorldClient& world, ChatClient& chat, GuiAccess* gui, DisplayAccess* display)
        : scene(scene), projectRoot(projectRoot), input(input), bus(bus), fsm(fsm), physics(physics), audio(audio),
          network(network), auth(auth), world(world), chat(chat), gui(gui), display(display)
    {
        for (const LoadedScene& loaded : scene.loadedScenes())
            scenePaths[loaded.slot] = loaded.slot == scene.activeSlot() ? scenePath : projectPath(loaded.path);
    }

    Scene& scene;
    std::optional<std::filesystem::path> projectRoot;
    std::map<SceneSlot, std::string> scenePaths; // each loaded scene's, project-relative; empty if unsaved or no project
    Input* input;
    EventBus& bus;
    StateMachines& fsm;
    Physics& physics;
    Audio& audio;
    Network& network;
    AuthClient& auth;
    WorldClient& world;
    ChatClient& chat;
    GuiAccess* gui; // null: no game GUIs here, kast.gui is nil
    DisplayAccess* display; // null: no window here, kast.display is nil
    std::unordered_map<entt::entity, AudioHandle> sources; // the last voice each AudioSource started (kast.audio.stopSource)
    MaterialLibrary materials; // shared .mat files, for kast.get/setMaterialProperty
    std::vector<SceneRequest> sceneRequests; // this frame's, in call order (a switch drops the ones before it)
    bool inShutdown = false;                 // kast.loadScene is ignored while a shutdown runs
    sol::state lua;        // declared before everything holding Lua references, so it is destroyed last
    sol::table api;        // kast.* functions, copied into each system's own read-only kast table
    sol::table inputApi;   // kast.input.*
    sol::table fsmApi;     // kast.fsm.*
    sol::table physicsApi; // kast.physics.*
    sol::table audioApi;   // kast.audio.*
    sol::table netApi;     // kast.net.*
    sol::table skyApi;     // kast.sky.*
    sol::table lightmapsApi; // kast.lightmaps.*
    sol::table sdfgiApi;     // kast.sdfgi.*
    sol::table ssaoApi;      // kast.ssao.*
    sol::table ssrApi;       // kast.ssr.*
    sol::table dofApi;       // kast.dof.*
    sol::table volumetricFogApi; // kast.volumetricFog.*
    sol::table particlesApi; // kast.particles.*
    sol::table authApi;    // kast.auth.*, only while Online Services are enabled
    sol::table worldApi;   // kast.world.*, likewise
    sol::table chatApi;    // kast.chat.*, likewise
    sol::table guiApi;     // kast.gui.*, only with a GuiAccess
    sol::table displayApi; // kast.display.*, only with a DisplayAccess
    DefinitionFiles fsmFiles; // state machine definition files at the last load, for hot reload
    std::string callbackPath; // the definition file whose enter/exit is running (log prefixes, event publisher)
    std::list<System> systems; // every loaded scene's, each path once (a list: removing one keeps the others' addresses)
    std::unordered_map<EntityId, entt::entity> ids; // ponytail: rebuilt after any entity creation; index in Scene if big
    uint64_t indexedAt = 0; // Scene::creations() when ids was built
    std::map<std::string, std::shared_ptr<const PrefabAsset>> prefabs; // kast.instantiate's, loaded once per session
    System* current = nullptr; // whose code is running: log prefixes, event publisher and subscription owner
    struct Request {           // a kast.world.request waiting for its world.reply
        System* owner;
        sol::protected_function confirmed, rejected; // either may be empty
    };
    std::map<uint32_t, Request> requests; // by requestId
    EventBus::Subscription replies;       // world.reply -> requests

    void init(const std::vector<std::string>& paths);
    void tick(float dt);
    void stop();
    void startSystem(System& system); // resolves its file and runs it (init)
    // Additive scenes: what kast.loadScene(path, {additive = true}) and kast.unloadScene do at the end of the frame.
    void addScene(SceneRequest& request);
    void unloadScene(const std::string& path);
    std::string projectPath(const std::string& file) const; // a LoadedScene path, project-relative if inside the project
    std::string luaScenePath(lua_State* L, const char* function, const std::string& path) const; // checked, normalized
    std::vector<std::string> loadedAfterRequests() const; // the loaded paths once this frame's requests apply
    SceneSlot slotOf(const std::string& path) const; // a loaded scene's, by project-relative path

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
    void loadItems();
    sol::table itemTable();
    std::map<std::string, nlohmann::json> items; // kast.items, from <project>/items/
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
    void bindSkyApi();
    void bindLightmapsApi();
    void bindSdfgiApi();
    sol::table postBlockApi(const char* luaName, const char* block);
    void bindParticlesApi();
    void bindAuthApi();
    void bindWorldApi();
    void bindGuiApi();
    void bindDisplayApi();
    // Starts entity's AudioSource. Throws std::invalid_argument (see Audio::play).
    AudioHandle playSource(entt::entity entity);
    // Every AudioSource with playOnStart (slot: that loaded scene's); a bad clip logs an error and plays nothing.
    void playOnStart(std::optional<SceneSlot> slot = std::nullopt);
};

void LuaSystems::State::init(const std::vector<std::string>& paths)
{
    openSandbox(lua);
    bindApi();
    indexIds();
    fsm.attach(scene, [this](EntityId id) { return find(id); });
    if (projectRoot) {
        loadMachines(false);
        loadItems();
    }
    for (const std::string& path : paths)
        systems.push_back({.path = path});
    for (System& system : systems)
        startSystem(system);
    indexIds(); // entities the inits created
    fsm.enterAll();
}

void LuaSystems::State::startSystem(System& system)
{
    std::string error = "no project (open a scene inside a project to run Lua systems)";
    const std::optional<std::filesystem::path> file =
        projectRoot ? resolveProjectPath(*projectRoot, system.path, error) : std::nullopt;
    if (!file)
        return disable(system, std::format("{}: {}", system.path, error));
    system.file = *file;
    std::error_code ec;
    system.mtime = std::filesystem::last_write_time(system.file, ec);
    load(system);
}

void LuaSystems::State::addScene(SceneRequest& request)
{
    std::string file = projectRoot ? (*projectRoot / std::filesystem::path(request.path)).lexically_normal().string()
                                   : request.path;
    const SceneSlot slot = appendScene(scene, std::move(request.scene), std::move(file));
    scenePaths[slot] = request.path;
    indexIds();
    log::info(std::format("Loaded scene '{}' additively", request.path));
    playOnStart(slot);
    for (const std::string& path : scene.settingsOf(slot).systems) // a script another loaded scene runs isn't started twice
        if (std::ranges::find(systems, path, &System::path) == systems.end())
            startSystem(systems.emplace_back(System{.path = path}));
    indexIds(); // entities the inits created
    fsm.syncAll(); // its StateMachine entities enter their initial states
}

void LuaSystems::State::unloadScene(const std::string& path)
{
    const SceneSlot slot = slotOf(path);
    std::vector<std::string> kept; // the other loaded scenes' systems keep running
    for (const LoadedScene& loaded : scene.loadedScenes())
        if (loaded.slot != slot)
            for (const std::string& system : scene.settingsOf(loaded.slot).systems)
                kept.push_back(system);
    for (const std::string& listed : scene.settingsOf(slot).systems) {
        const auto it = std::ranges::find(systems, listed, &System::path);
        if (it == systems.end() || std::ranges::find(kept, listed) != kept.end())
            continue;
        if (it->ok && it->shutdown)
            call(*it, it->shutdown, "shutdown");
        std::erase_if(requests, [&](const auto& entry) { return entry.second.owner == &*it; });
        systems.erase(it);
    }
    scene.removeScene(slot);
    scenePaths.erase(slot);
    for (auto it = sources.begin(); it != sources.end();) // the unloaded entities' voices stop with them
        if (!scene.valid(it->first)) {
            audio.stop(it->second);
            it = sources.erase(it);
        } else {
            ++it;
        }
    indexIds();
    log::info(std::format("Unloaded scene '{}'", path));
}

std::string LuaSystems::State::projectPath(const std::string& file) const
{
    if (file.empty() || !projectRoot)
        return file;
    std::filesystem::path path(file);
    if (path.is_relative())
        path = *projectRoot / path;
    return toProjectPath(*projectRoot, path).value_or(file);
}

std::string LuaSystems::State::luaScenePath(lua_State* L, const char* function, const std::string& path) const
{
    if (!projectRoot)
        raise(L, std::format("{}: no project is active", function));
    std::string error;
    const std::optional<std::filesystem::path> file = resolveProjectPath(*projectRoot, path, error);
    if (!file)
        raise(L, std::format("{}: {}", function, error));
    return toProjectPath(*projectRoot, *file).value_or(path);
}

std::vector<std::string> LuaSystems::State::loadedAfterRequests() const
{
    std::vector<std::string> loaded;
    for (const LoadedScene& each : scene.loadedScenes())
        loaded.push_back(scenePaths.at(each.slot));
    for (const SceneRequest& request : sceneRequests) {
        if (request.kind == SceneRequest::Kind::Switch)
            loaded = {request.path};
        else if (request.kind == SceneRequest::Kind::Add)
            loaded.push_back(request.path);
        else if (request.kind == SceneRequest::Kind::Unload)
            std::erase(loaded, request.path);
    }
    return loaded;
}

SceneSlot LuaSystems::State::slotOf(const std::string& path) const
{
    for (const auto& [slot, loaded] : scenePaths)
        if (loaded == path)
            return slot;
    return scene.activeSlot(); // never: requests are checked against loadedAfterRequests
}

void LuaSystems::State::tick(float dt)
{
    if (Scene::creations() != indexedAt)
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
    indexedAt = Scene::creations();
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

// kast.items (lua_items.h): rejected files are logged; the valid ones are what Lua sees.
void LuaSystems::State::loadItems()
{
    ItemDefinitions defs = loadItemDefinitions(lua, *projectRoot);
    for (const std::string& error : defs.errors)
        log::error(std::format("Item rejected: {}", error));
    items = std::move(defs.items);
}

// {[id] = {id =, name =, ...}}: read-only all the way down, a fresh copy per environment.
sol::table LuaSystems::State::itemTable()
{
    sol::table table = lua.create_table();
    for (const auto& [id, item] : items)
        table[id] = readOnly(jsonToLua(lua, item).as<sol::table>());
    return readOnly(table);
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
    kast["sky"] = readOnly(skyApi);
    kast["lightmaps"] = readOnly(lightmapsApi);
    kast["sdfgi"] = readOnly(sdfgiApi);
    kast["ssao"] = readOnly(ssaoApi);
    kast["ssr"] = readOnly(ssrApi);
    kast["dof"] = readOnly(dofApi);
    kast["volumetricFog"] = readOnly(volumetricFogApi);
    kast["particles"] = readOnly(particlesApi);
    kast["items"] = itemTable();
    if (auth.enabled())
        kast["auth"] = readOnly(authApi);
    if (world.enabled())
        kast["world"] = readOnly(worldApi);
    if (chat.enabled())
        kast["chat"] = readOnly(chatApi);
    if (gui)
        kast["gui"] = readOnly(guiApi);
    if (display)
        kast["display"] = readOnly(displayApi);
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
    // A shader's params are known once the library has a root, which the renderer gives it on its first frame; a
    // system's first update can run before that (Play's first frame), so give it the project root here.
    if (shaders().root().empty() && projectRoot)
        shaders().setRoot(*projectRoot);
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
        for (const FieldDesc& field : type.fields()) // an omitDefault field at its default isn't in the JSON: nil
            if (const auto value = json.find(field.name); value != json.end())
                table[field.name] = fieldJsonToLua(lua, field, *value);
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
    // Failures a game can recover from (bad path, missing or broken file) return nil and log why; bad argument types
    // raise like the other functions.
    api.set_function("instantiate", [this, &registry](sol::this_state L, const sol::object& path, const sol::object& parent,
                                                      const sol::object& position) -> sol::object {
        const auto fail = [&](const std::string& why) {
            log::warn(std::format("{}: kast.instantiate: {}", who(), why));
            return sol::make_object(lua, sol::lua_nil);
        };
        if (path.get_type() != sol::type::string)
            raise(L, std::format("kast.instantiate: path must be a string, got {}", toString(L, path)));
        const bool root = parent.get_type() == sol::type::lua_nil || parent.get_type() == sol::type::none;
        const entt::entity parentEntity = root ? entt::null : entity(L, parent);
        std::optional<bx::Vec3> at;
        if (position.get_type() == sol::type::table) {
            const sol::table t = position.as<sol::table>();
            const sol::object x = t["x"], y = t["y"], z = t["z"];
            if (x.get_type() != sol::type::number || y.get_type() != sol::type::number || z.get_type() != sol::type::number)
                raise(L, "kast.instantiate: position must be {x =, y =, z =}");
            at = bx::Vec3{x.as<float>(), y.as<float>(), z.as<float>()};
        } else if (position.get_type() != sol::type::lua_nil && position.get_type() != sol::type::none) {
            raise(L, std::format("kast.instantiate: position must be {{x =, y =, z =}}, got {}", toString(L, position)));
        }
        const std::string file = path.as<std::string>();
        if (!projectRoot)
            return fail("no project is active");
        std::shared_ptr<const PrefabAsset>& asset = prefabs[file];
        std::string error;
        if (!asset && !loadPrefab(*projectRoot, file, asset, error)) {
            prefabs.erase(file);
            return fail(error);
        }
        const entt::entity e = instantiatePrefab(scene, asset, parentEntity, error);
        if (e == entt::null)
            return fail(error);
        if (at)
            registry.get<Transform>(e).position = *at;
        auto index = [&](auto& self, entt::entity created) -> void { // usable by id right away, as kast.create's
            ids[registry.get<PersistentId>(created).value] = created;
            for (entt::entity child : scene.children(created))
                self(self, child);
        };
        index(index, e);
        fsm.syncAll(); // StateMachines in the prefab enter their initial states
        return sol::make_object(lua, registry.get<PersistentId>(e).value);
    });

    // Zone objects (docs/engine/terrain.html#objects): the game binds the keys. kast.nearestInteractable(position,
    // maxDistance) -> {id, name, pos = {x, y, z}, distance} of the streamed interactable nearest by the horizontal
    // distance to its shape's edge, or nil. kast.interact(id) sends Interact while in the world (true), else false;
    // the server's Lua answers, if it does, with an event.
    api.set_function("nearestInteractable", [this](sol::this_state L, const sol::object& position,
                                                   const sol::object& maxDistance) -> sol::object {
        const sol::table t = position.get_type() == sol::type::table ? position.as<sol::table>() : sol::table();
        const sol::object x = t.valid() ? t["x"] : sol::object(), z = t.valid() ? t["z"] : sol::object();
        if (x.get_type() != sol::type::number || z.get_type() != sol::type::number)
            raise(L, std::format("kast.nearestInteractable: position must be {{x =, y =, z =}}, got {}", toString(L, position)));
        if (maxDistance.get_type() != sol::type::number || !(maxDistance.as<double>() >= 0))
            raise(L, std::format("kast.nearestInteractable: maxDistance must be a number >= 0, got {}", toString(L, maxDistance)));
        const auto found = nearestInteractable(scene, x.as<float>(), z.as<float>(), maxDistance.as<float>());
        if (!found)
            return sol::make_object(lua, sol::lua_nil);
        const map::ZoneObject& o = *found->object;
        return sol::make_object(lua, lua.create_table_with("id", o.id, "name", o.name, "distance", found->distance, "pos",
                                                           lua.create_table_with("x", o.pos[0], "y", o.pos[1], "z", o.pos[2])));
    });
    api.set_function("interact", [this](sol::this_state L, const sol::object& id) {
        const double n = id.get_type() == sol::type::number ? id.as<double>() : 0.0;
        if (!(n >= 1 && n <= 4294967295.0 && n == std::floor(n)))
            raise(L, std::format("kast.interact: id must be a whole number from 1 to 4294967295, got {}", toString(L, id)));
        if (!world.enabled() || world.state() != WorldState::InWorld)
            return false;
        world.interact(static_cast<uint32_t>(n));
        return true;
    });

    // Per-entity material properties (Unity's renderer.material): the first set copies the entity's MeshRenderer
    // material into a MaterialInstance only it renders with; reads come from the instance, else the shared .mat.
    // Fields and checks as in .mat files, plus the params of its .kshader; colors as {r, g, b[, a]} or
    // {x =, y =, z =[, w =]}; a texture2D param as its path.
    api.set_function("getMaterialProperty", [this](sol::this_state L, const sol::object& id, const std::string& name) {
        const entt::entity e = entity(L, id);
        const std::optional<Json> value = materialProperty(material(L, e), name);
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
        if (!setMaterialProperty(copy, name, materialValueToJson(value), error))
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

    // Validated now (raises at the calling line); applied by LuaSystems::tick after every update has run, in call
    // order (docs/engine/additive-scenes.html). A switch (no {additive = true}) drops the requests before it.
    api.set_function("loadScene", [this](sol::this_state L, const std::string& path, const sol::object& options) {
        if (inShutdown)
            return; // the session is ending
        bool additive = false;
        if (options.get_type() == sol::type::table) {
            const sol::object flag = options.as<sol::table>()["additive"];
            if (flag.get_type() != sol::type::lua_nil && flag.get_type() != sol::type::boolean)
                raise(L, std::format("kast.loadScene: additive must be a boolean, got {}", toString(L, flag)));
            additive = flag.get_type() == sol::type::boolean && flag.as<bool>();
        } else if (options.get_type() != sol::type::lua_nil && options.get_type() != sol::type::none) {
            raise(L, std::format("kast.loadScene: options must be a table, got {}", toString(L, options)));
        }
        const std::string relative = luaScenePath(L, "kast.loadScene", path);
        if (const std::vector<std::string> loaded = loadedAfterRequests();
            additive && std::ranges::find(loaded, relative) != loaded.end())
            raise(L, std::format("kast.loadScene: '{}' is already loaded", relative));
        std::string error;
        Scene loaded;
        if (!loadScene(resolveProjectPath(*projectRoot, relative, error)->string(), loaded, error, nullptr, *projectRoot))
            raise(L, std::format("kast.loadScene: {}", error));
        if (!additive)
            sceneRequests.clear(); // the last switch wins
        sceneRequests.push_back({additive ? SceneRequest::Kind::Add : SceneRequest::Kind::Switch, relative, std::move(loaded)});
    });
    api.set_function("unloadScene", [this](sol::this_state L, const std::string& path) {
        if (inShutdown)
            return;
        const std::string relative = luaScenePath(L, "kast.unloadScene", path);
        const std::vector<std::string> loaded = loadedAfterRequests();
        if (std::ranges::find(loaded, relative) == loaded.end())
            raise(L, std::format("kast.unloadScene: '{}' is not loaded", relative));
        if (loaded.size() == 1)
            raise(L, std::format("kast.unloadScene: '{}' is the only loaded scene (kast.loadScene replaces it)", relative));
        sceneRequests.push_back({SceneRequest::Kind::Unload, relative});
    });
    api.set_function("setActiveScene", [this](sol::this_state L, const std::string& path) {
        if (inShutdown)
            return;
        const std::string relative = luaScenePath(L, "kast.setActiveScene", path);
        const std::vector<std::string> loaded = loadedAfterRequests();
        if (std::ranges::find(loaded, relative) == loaded.end())
            raise(L, std::format("kast.setActiveScene: '{}' is not loaded", relative));
        sceneRequests.push_back({SceneRequest::Kind::Activate, relative});
    });
    const auto activeScene = [this]() -> sol::optional<std::string> {
        const std::string& path = scenePaths.at(scene.activeSlot());
        return path.empty() ? sol::nullopt : sol::optional<std::string>(path);
    };
    api.set_function("currentScene", activeScene);
    api.set_function("activeScene", activeScene);
    api.set_function("loadedScenes", [this]() {
        sol::table list = lua.create_table();
        for (const LoadedScene& loaded : scene.loadedScenes())
            list.add(scenePaths.at(loaded.slot));
        return list;
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
    api.set_function("subscribe", [this](sol::this_state L, const sol::object& name, const sol::object& fn,
                                         const sol::object& options) {
        std::string event = eventName(L, "kast.subscribe", name);
        if (fn.get_type() != sol::type::function)
            raise(L, std::format("kast.subscribe: callback must be a function, got {}", toString(L, fn)));
        if (options != sol::lua_nil && options.get_type() != sol::type::table)
            raise(L, std::format("kast.subscribe: options must be a table, got {}", toString(L, options)));
        if (!current)
            raise(L, "kast.subscribe: only a system can subscribe");
        const bool replay = options != sol::lua_nil && options.as<sol::table>().get_or("replay", false);
        System* owner = current;
        std::string what = std::format("event '{}'", event);
        // on the main thread: fn's own may be a coroutine, collected before the drain
        sol::protected_function callback(lua.lua_state(), fn);
        const nlohmann::json* missed = replay && world.enabled() ? world.latest(event) : nullptr;
        EventBus::Subscription subscription =
            bus.subscribe(std::move(event), [this, owner, what, callback](const nlohmann::json& payload) {
                if (owner->ok) // a disabled system's subscriptions are gone; this guards the rest of its drain
                    call(*owner, callback, what, jsonToLua(lua, payload));
            });
        const uint64_t id = subscription.id();
        owner->subscriptions.push_back(std::move(subscription));
        if (missed) { // the world connection's last one, right away: a scene loaded after it catches up
            sol::protected_function_result result = callback(jsonToLua(lua, *missed));
            if (!result.valid()) // inside the caller's own call: its error, at this line
                raise(L, std::format("kast.subscribe: replaying {}: {}", what, result.get<sol::error>().what()));
        }
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
        const Camera& c = scene.registry().get<Camera>(camera);
        const float aspect = input->view.width / input->view.height;
        const float ndcX = input->mouseX * 2.0f - 1.0f;
        const float ndcY = 1.0f - input->mouseY * 2.0f;
        const auto table = [&](const bx::Vec3& v) {
            return sol::make_object(L, lua.create_table_with("x", v.x, "y", v.y, "z", v.z));
        };
        if (c.projection == CameraProjection::Orthographic) {
            // Parallel rays along the camera's forward, from the cursor's point on the near plane (scale ignored).
            const auto axis = [&](const bx::Vec3& v) { return bx::normalize(transformDir(v, world)); };
            const bx::Vec3 forward = axis({0.0f, 0.0f, -1.0f});
            bx::Vec3 origin{world[12], world[13], world[14]};
            origin = bx::mad(axis({1.0f, 0.0f, 0.0f}), ndcX * c.orthographicSize * aspect, origin);
            origin = bx::mad(axis({0.0f, 1.0f, 0.0f}), ndcY * c.orthographicSize, origin);
            return {table(bx::mad(forward, c.nearPlane, origin)), table(forward)};
        }
        const float tanHalf = std::tan(bx::toRad(c.verticalFovDegrees) * 0.5f);
        const bx::Vec3 local = {ndcX * tanHalf * aspect, ndcY * tanHalf, -1.0f}; // the camera looks down -z
        const bx::Vec3 dir = bx::normalize(transformDir(local, world));
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
    bindSkyApi();
    bindLightmapsApi();
    bindSdfgiApi();
    ssaoApi = postBlockApi("ssao", "ssao");
    ssrApi = postBlockApi("ssr", "ssr");
    dofApi = postBlockApi("dof", "depthOfField");
    volumetricFogApi = postBlockApi("volumetricFog", "volumetricFog");
    bindParticlesApi();
    bindAuthApi();
    bindWorldApi();
    chatApi = makeChatApi(lua, chat); // kast.chat.*: see lua_chat_api.h
    bindGuiApi();
    bindDisplayApi();
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
    // An entity destroyed this frame keeps its body until the next physics step (e.g. a terrain tile of the map just
    // left): queries leave it out.
    const auto gone = [this](entt::entity e) { return !scene.registry().valid(e) || !scene.registry().all_of<PersistentId>(e); };
    const auto idList = [this, idOf, gone](const std::vector<entt::entity>& entities) {
        sol::table table = lua.create_table();
        for (const entt::entity e : entities)
            if (!gone(e))
                table[table.size() + 1] = idOf(e);
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
    physicsApi.set_function("raycast", [this, vec3, number, toLua, idOf, gone, guarded](
                                           sol::this_state L, const sol::object& origin, const sol::object& direction,
                                           const sol::object& maxDistance) -> sol::object {
        const std::string fn = "kast.physics.raycast";
        const bx::Vec3 from = vec3(L, fn, "origin", origin);
        const bx::Vec3 dir = vec3(L, fn, "direction", direction);
        const float distance = number(L, fn, "maxDistance", maxDistance);
        const auto hit = guarded(L, fn, [&] { return physics.raycast(from, dir, distance); });
        if (!hit || gone(hit->entity))
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
    physicsApi.set_function("getGround", [this, toLua, idOf, gone, guarded](sol::this_state L,
                                                                      const sol::object& id) -> sol::object {
        const entt::entity e = entity(L, id);
        const auto ground = guarded(L, "kast.physics.getGround", [&] { return physics.ground(e); });
        if (!ground || gone(ground->entity))
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

void LuaSystems::State::playOnStart(std::optional<SceneSlot> slot)
{
    scene.forEachDepthFirst([&](entt::entity e) {
        const AudioSource* source = scene.registry().try_get<AudioSource>(e);
        if (!source || !source->playOnStart || (slot && scene.sceneOf(e) != *slot))
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
    // The functions that act on one document's element (value, setValue, setText, setRichText, setVisible,
    // setAttribute) also take an entity id first: one argument more than their own and a number first target that
    // entity's WorldUI document (GuiAccess::writeEntity); anything else is their own form, its errors as they were.
    const auto arg = [](const sol::variadic_args& args, size_t i) {
        return i < args.size() ? args.get<sol::object>(static_cast<int>(i)) : sol::make_object(args.lua_state(), sol::lua_nil);
    };
    const auto target = [=, this](lua_State* L, const sol::variadic_args& args, size_t own) -> std::optional<EntityId> {
        if (args.size() <= own || arg(args, 0).get_type() != sol::type::number)
            return std::nullopt;
        return scene.registry().get<PersistentId>(entity(L, arg(args, 0))).value;
    };
    guiApi = lua.create_table();
    guiApi.set_function("value", [=, this](sol::this_state L, sol::variadic_args args) -> sol::object {
        const std::optional<EntityId> e = target(L, args, 1);
        const std::string id = text(L, "value", "id", arg(args, e ? 1 : 0));
        const std::optional<GuiValue> value = e ? gui->entityValue(*e, id) : gui->value(id);
        return value ? jsonToLua(lua, *value) : sol::object(sol::lua_nil);
    });
    guiApi.set_function("setValue", [=, this](sol::this_state L, sol::variadic_args args) {
        const std::optional<EntityId> e = target(L, args, 2);
        const std::string widget = text(L, "setValue", "id", arg(args, e ? 1 : 0));
        const sol::object value = arg(args, e ? 2 : 1);
        GuiValue json;
        switch (value.get_type()) {
        case sol::type::boolean: json = value.as<bool>(); break;
        case sol::type::number: json = value.as<double>(); break;
        case sol::type::string: json = value.as<std::string>(); break;
        case sol::type::lua_nil:
        case sol::type::none: json = nullptr; break; // a list: no row selected
        default:
            raise(L, std::format("kast.gui.setValue: value must be a boolean, number, string or nil, got {}", toString(L, value)));
        }
        e ? gui->writeEntity(*e, widget, {.value = std::move(json)}) : gui->setValue(widget, std::move(json));
    });
    guiApi.set_function("scrollTo", [=, this](sol::this_state L, const sol::object& id, const sol::object& index) {
        const std::string list = text(L, "scrollTo", "id", id);
        const double row = index.get_type() == sol::type::number ? index.as<double>() : -1.0;
        if (index.get_type() != sol::type::number || row != std::floor(row) || std::abs(row) > 2147483647.0)
            raise(L, std::format("kast.gui.scrollTo: index must be a whole number, got {}", toString(L, index)));
        gui->scrollTo(list, static_cast<int>(row)); // a negative one is out of range: logged there
    });
    guiApi.set_function("scrollToEnd", [=, this](sol::this_state L, const sol::object& id) {
        gui->scrollToEnd(text(L, "scrollToEnd", "id", id));
    });
    guiApi.set_function("setText", [=, this](sol::this_state L, sol::variadic_args args) {
        const std::optional<EntityId> e = target(L, args, 2);
        const std::string id = text(L, "setText", "id", arg(args, e ? 1 : 0));
        std::string value = text(L, "setText", "text", arg(args, e ? 2 : 1));
        e ? gui->writeEntity(*e, id, {.text = std::move(value)}) : gui->setText(id, value);
    });
    guiApi.set_function("setRichText", [=, this](sol::this_state L, sol::variadic_args args) {
        const std::optional<EntityId> e = target(L, args, 2);
        const std::string id = text(L, "setRichText", "id", arg(args, e ? 1 : 0));
        std::string value = text(L, "setRichText", "text", arg(args, e ? 2 : 1));
        e ? gui->writeEntity(*e, id, {.rich = std::move(value)}) : gui->setRichText(id, value);
    });
    guiApi.set_function("setVisible", [=, this](sol::this_state L, sol::variadic_args args) {
        const std::optional<EntityId> e = target(L, args, 2);
        const sol::object visible = arg(args, e ? 2 : 1);
        if (visible.get_type() != sol::type::boolean)
            raise(L, std::format("kast.gui.setVisible: visible must be a boolean, got {}", toString(L, visible)));
        const std::string id = text(L, "setVisible", "id", arg(args, e ? 1 : 0));
        e ? gui->writeEntity(*e, id, {.visible = visible.as<bool>()}) : gui->setVisible(id, visible.as<bool>());
    });
    guiApi.set_function("setAttribute", [=, this](sol::this_state L, sol::variadic_args args) {
        const std::optional<EntityId> e = target(L, args, 3);
        const size_t at = e ? 1 : 0;
        const std::string id = text(L, "setAttribute", "id", arg(args, at));
        const std::string name = text(L, "setAttribute", "name", arg(args, at + 1));
        std::string value = text(L, "setAttribute", "value", arg(args, at + 2));
        e ? gui->writeEntity(*e, id, {.attributes = {{name, std::move(value)}}}) : gui->setAttribute(id, name, value);
    });
    guiApi.set_function("focus", [=, this](sol::this_state L, const sol::object& id) {
        return gui->focus(text(L, "focus", "id", id));
    });
    guiApi.set_function("focusedField", [this]() -> sol::object {
        const std::optional<std::string> id = gui->focusedField();
        return id ? sol::make_object(lua, *id) : sol::object(sol::lua_nil);
    });
    guiApi.set_function("mouseOver", [this] { return gui->mouseOver(); });
    // kast.gui.worldText(pos, text, class, lifetime): a floating text at a world position (GuiAccess::worldText);
    // kast.gui.worldTextCount(): how many show.
    guiApi.set_function("worldText", [=, this](sol::this_state L, const sol::object& pos, const sol::object& value,
                                               const sol::object& cssClass, const sol::object& lifetime) {
        std::array<float, 3> at{};
        bool numbers = pos.get_type() == sol::type::table;
        for (int i = 0; numbers && i < 3; ++i) {
            const sol::object c = pos.as<sol::table>()[std::string(1, "xyz"[i])];
            numbers = c.get_type() == sol::type::number;
            at[i] = numbers ? c.as<float>() : 0.0f;
        }
        if (!numbers)
            raise(L, std::format("kast.gui.worldText: pos must be a table {{x, y, z}} of numbers, got {}", toString(L, pos)));
        const std::string shown = text(L, "worldText", "text", value);
        const std::string classes = text(L, "worldText", "class", cssClass);
        if (lifetime.get_type() != sol::type::number || !(lifetime.as<double>() > 0.0))
            raise(L, std::format("kast.gui.worldText: lifetime must be a positive number of seconds, got {}",
                                 toString(L, lifetime)));
        gui->worldText(at, shown, classes, lifetime.as<float>());
    });
    guiApi.set_function("worldTextCount", [this] { return gui->worldTextCount(); });
    // Data models (data-model="name" in an .rml): key is a top-level variable, value plain data (luaToJson).
    const auto key = [=](lua_State* L, const char* function, const sol::object& value) {
        std::string name = text(L, function, "key", value);
        if (!guiVariableName(name))
            raise(L, std::format("kast.gui.{}: key must be a letter then letters, digits or _ (not an RmlUi reserved "
                                 "name), got '{}'", function, name));
        return name;
    };
    guiApi.set_function("set", [=, this](sol::this_state L, const sol::object& model, const sol::object& name,
                                         const sol::object& value) {
        const std::string m = text(L, "set", "model", model);
        const std::string k = key(L, "set", name);
        GuiValue json;
        std::string error;
        if (!luaToJson(value, json, error, "value"))
            raise(L, "kast.gui.set: " + error);
        gui->setModelValue(m, k, std::move(json));
    });
    guiApi.set_function("get", [=, this](sol::this_state L, const sol::object& model, const sol::object& name) {
        const std::optional<GuiValue> value = gui->modelValue(text(L, "get", "model", model), key(L, "get", name));
        return value ? jsonToLua(lua, *value) : sol::object(sol::lua_nil);
    });
    // Live elements (gone when their document reloads); a missing id or a taken new id logs and returns false.
    guiApi.set_function("clone", [=, this](sol::this_state L, const sol::object& templateId, const sol::object& parentId,
                                           const sol::object& newId) {
        return gui->cloneElement(text(L, "clone", "templateId", templateId), text(L, "clone", "parentId", parentId),
                                 text(L, "clone", "newId", newId));
    });
    guiApi.set_function("instantiate", [=, this](sol::this_state L, const sol::object& prefab, const sol::object& parentId,
                                                 const sol::object& newId) {
        return gui->instantiatePrefab(text(L, "instantiate", "prefabPath", prefab), text(L, "instantiate", "parentId", parentId),
                                      text(L, "instantiate", "newId", newId));
    });
    guiApi.set_function("create", [=, this](sol::this_state L, const sol::object& parentId, const sol::object& tag,
                                            const sol::object& newId, const sol::object& attrs) {
        const std::string parent = text(L, "create", "parentId", parentId);
        const std::string t = text(L, "create", "tag", tag);
        const std::string id = text(L, "create", "newId", newId);
        std::map<std::string, std::string> attributes;
        if (attrs.get_type() == sol::type::table) {
            for (const auto& [name, value] : attrs.as<sol::table>())
                attributes[text(L, "create", "attribute names", name)] = text(L, "create", "attribute values", value);
        } else if (attrs.get_type() != sol::type::lua_nil && attrs.get_type() != sol::type::none) {
            raise(L, std::format("kast.gui.create: attrs must be a table or nil, got {}", toString(L, attrs)));
        }
        return gui->createElement(parent, t, id, attributes);
    });
    guiApi.set_function("remove", [=, this](sol::this_state L, const sol::object& id) {
        return gui->removeElement(text(L, "remove", "id", id));
    });
    guiApi.set_function("hovered", [this]() -> sol::object {
        const std::optional<std::string> id = gui->hovered();
        return id ? sol::make_object(lua, *id) : sol::object(sol::lua_nil);
    });
    // Scaling (docs/engine/game-gui.html#scaling): out-of-range numbers are clamped (logged), wrong types raise.
    const auto number = [](lua_State* L, const char* function, const char* what, const sol::object& value) {
        const double n = value.get_type() == sol::type::number ? value.as<double>() : std::nan("");
        if (!std::isfinite(n))
            raise(L, std::format("kast.gui.{}: {} must be a number, got {}", function, what, toString(L, value)));
        return n;
    };
    const auto clamped = [](const char* function, const char* what, double value, double min, double max) {
        const double out = std::clamp(value, min, max);
        if (out != value)
            log::warn(std::format("kast.gui.{}: {} {} clamped to {}", function, what, value, out));
        return out;
    };
    guiApi.set_function("setScale", [=, this](sol::this_state L, const sol::object& value) {
        GuiScale scale = gui->scale();
        scale.uiScale = clamped("setScale", "scale", number(L, "setScale", "scale", value), kMinUiScale, kMaxUiScale);
        gui->setScale(scale);
    });
    guiApi.set_function("scale", [this] { return gui->scale().uiScale; });
    guiApi.set_function("dpRatio", [this] { return gui->dpRatio(); });
    // (w, h[, match]) sets it, match kept if nil; (nil) unsets it.
    guiApi.set_function("setReferenceResolution", [=, this](sol::this_state L, const sol::object& w, const sol::object& h,
                                                            const sol::object& match) {
        GuiScale scale = gui->scale();
        const auto none = [](const sol::object& value) {
            return value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none;
        };
        if (none(w) && none(h)) {
            scale.referenceResolution.reset();
        } else {
            const auto side = [&](const char* what, const sol::object& value) {
                const double n = number(L, "setReferenceResolution", what, value);
                if (n < 1.0 || n > 16384.0 || n != std::floor(n))
                    raise(L, std::format("kast.gui.setReferenceResolution: {} must be a whole number from 1 to 16384, got {}",
                                         what, toString(L, value)));
                return static_cast<int>(n);
            };
            scale.referenceResolution = std::pair{side("width", w), side("height", h)};
        }
        if (!none(match))
            scale.match = clamped("setReferenceResolution", "match", number(L, "setReferenceResolution", "match", match), 0.0, 1.0);
        gui->setScale(scale);
    });
    guiApi.set_function("referenceResolution", [this](sol::this_state L) -> sol::variadic_results {
        sol::variadic_results out;
        const GuiScale scale = gui->scale();
        if (!scale.referenceResolution) {
            out.push_back(sol::make_object(L, sol::lua_nil));
            return out;
        }
        out.push_back(sol::make_object(L, scale.referenceResolution->first));
        out.push_back(sol::make_object(L, scale.referenceResolution->second));
        out.push_back(sol::make_object(L, scale.match));
        return out;
    });
}

// kast.display.*: the game's window through DisplayAccess (docs/engine/game-gui.html#display). Wrong types raise; a
// mode the display can't do returns false (logged) and changes nothing.
void LuaSystems::State::bindDisplayApi()
{
    const auto modeTable = [this](const DisplayMode& mode) {
        sol::table table = lua.create_table();
        table["mode"] = windowModeName(mode.mode);
        table["width"] = mode.width;
        table["height"] = mode.height;
        if (mode.refresh)
            table["refresh"] = *mode.refresh;
        return table;
    };
    displayApi = lua.create_table();
    // {mode=, width=, height=, refresh=}: each optional, the current mode's where left out (refresh: the highest).
    displayApi.set_function("setMode", [=, this](sol::this_state L, const sol::object& fields) {
        if (fields.get_type() != sol::type::table)
            raise(L, std::format("kast.display.setMode: expected a table {{mode=, width=, height=, refresh=}}, got {}",
                                 toString(L, fields)));
        DisplayMode want = display->mode();
        want.refresh.reset();
        std::string modeName;
        for (const auto& [key, value] : fields.as<sol::table>()) {
            const std::string name = key.get_type() == sol::type::string ? key.as<std::string>() : "";
            if (name == "mode") {
                if (value.get_type() != sol::type::string)
                    raise(L, std::format("kast.display.setMode: mode must be a string, got {}", toString(L, value)));
                modeName = value.as<std::string>();
            } else if (name == "width" || name == "height" || name == "refresh") {
                const double n = value.get_type() == sol::type::number ? value.as<double>() : 0.0;
                if (!(n >= 1.0 && n <= 16384.0 && n == std::floor(n)))
                    raise(L, std::format("kast.display.setMode: {} must be a whole number from 1 to 16384, got {}", name,
                                         toString(L, value)));
                (name == "width" ? want.width : name == "height" ? want.height : want.refresh.emplace()) = static_cast<int>(n);
            } else {
                raise(L, std::format("kast.display.setMode: unknown field {}", toString(L, key)));
            }
        }
        if (!modeName.empty()) {
            const std::optional<WindowMode> mode = windowModeFromName(modeName);
            if (!mode) {
                log::warn(std::format("kast.display.setMode: unknown mode '{}' (windowed, borderless or exclusive): unchanged",
                                      modeName));
                return false;
            }
            want.mode = *mode;
        }
        return display->setMode(want);
    });
    displayApi.set_function("mode", [=, this] { return modeTable(display->mode()); });
    displayApi.set_function("modes", [this] {
        sol::table list = lua.create_table();
        for (const DisplayModeInfo& mode : display->modes())
            list.add(lua.create_table_with("width", mode.width, "height", mode.height, "refresh", mode.refresh));
        return list;
    });
    displayApi.set_function("displayScale", [this] { return display->displayScale(); });
}

// kast.sky.*: the scene's sky as its "sky" block (scene_io.h), but colors as {x=, y=, z=} like kast.get's Vec3 fields.
// Runtime only: never saved, and Stop restores the scene's own.
// kast.particles.*(entity, ...): runtime control of an entity's ParticleEmitter (see particles.h); an invalid entity, one
// without an emitter or a bad argument raises.
void LuaSystems::State::bindParticlesApi()
{
    // The entity's emitter and its state (made on first use, as particleSystem would).
    const auto emitter = [this](lua_State* L, const char* name, const sol::object& id) {
        const entt::entity e = entity(L, id);
        auto* found = scene.registry().try_get<ParticleEmitter>(e);
        if (!found)
            raise(L, std::format("kast.particles.{}: entity {} has no ParticleEmitter", name, toString(L, id)));
        return std::pair<ParticleEmitter&, ParticleEmitterState&>{*found, emitterState(scene.registry(), e)};
    };

    particlesApi = lua.create_table();
    particlesApi.set_function("play", [emitter](sol::this_state L, const sol::object& id) {
        playEmitter(emitter(L, "play", id).second);
    });
    particlesApi.set_function("stop", [emitter](sol::this_state L, const sol::object& id, const sol::object& clear) {
        const bool none = clear.get_type() == sol::type::lua_nil || clear.get_type() == sol::type::none;
        if (!none && clear.get_type() != sol::type::boolean)
            raise(L, std::format("kast.particles.stop: clear must be true, false or nil, got {}", toString(L, clear)));
        stopEmitter(emitter(L, "stop", id).second, !none && clear.as<bool>());
    });
    particlesApi.set_function("burst", [emitter](sol::this_state L, const sol::object& id, const sol::object& n) {
        const double count = n.get_type() == sol::type::number ? n.as<double>() : -1.0;
        if (!(count >= 0.0 && count <= kMaxExactInteger && count == std::floor(count)))
            raise(L, std::format("kast.particles.burst: count must be a whole number >= 0, got {}", toString(L, n)));
        auto [found, state] = emitter(L, "burst", id);
        return burstEmitter(found, state, static_cast<int>(std::min<double>(count, kMaxParticles)));
    });
    particlesApi.set_function("alive", [emitter](sol::this_state L, const sol::object& id) {
        auto [found, state] = emitter(L, "alive", id);
        return aliveParticles(found, state);
    });
}

void LuaSystems::State::bindSkyApi()
{
    const FieldDesc color{.name = "", .kind = FieldKind::Vec3};
    skyApi = lua.create_table();
    skyApi.set_function("get", [this, color] {
        const nlohmann::ordered_json json = skyToJson(scene.sky);
        sol::table table = jsonToLua(lua, json).as<sol::table>();
        for (const char* key : {"top", "horizon", "bottom"})
            table[key] = fieldJsonToLua(lua, color, json[key]);
        return table;
    });
    // Partial tables: listed fields (and faces) change, the rest keep their values. All-or-nothing: anything the scene
    // file would reject raises and leaves the sky unchanged.
    skyApi.set_function("set", [this, color](sol::this_state L, const sol::object& fields) {
        if (fields.get_type() != sol::type::table)
            raise(L, std::format("kast.sky.set: expected a table, got {}", toString(L, fields)));
        nlohmann::ordered_json json = skyToJson(scene.sky);
        std::string error;
        for (const auto& [key, value] : fields.as<sol::table>()) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("kast.sky.set: field names must be strings, got {}", toString(L, key)));
            const std::string name = key.as<std::string>();
            nlohmann::json plain;
            if (name == "top" || name == "horizon" || name == "bottom") {
                if (!luaToFieldJson({.name = name, .kind = color.kind}, value, json[name], error))
                    raise(L, "kast.sky.set: " + error);
            } else if (!luaToJson(value, plain, error)) {
                raise(L, std::format("kast.sky.set: \"{}\": {}", name, error));
            } else if (name == "faces" && plain.is_object()) {
                for (const auto& [face, path] : plain.items())
                    json["faces"][face] = path;
            } else {
                json[name] = plain; // an unknown field or a bad value: skyFromJson says which
            }
        }
        SceneSky sky;
        if (!skyFromJson(json, sky, error))
            raise(L, "kast.sky.set: " + error);
        scene.sky = sky;
    });
}

// kast.lightmaps.get() / set{...}: the scene's lightmap settings (docs/engine/lightmaps.html). Only the runtime ones
// (enabled, intensity, directional) can be set: the others need a re-bake, which Lua can't start. All-or-nothing, as
// kast.sky.set.
void LuaSystems::State::bindLightmapsApi()
{
    lightmapsApi = lua.create_table();
    lightmapsApi.set_function("get", [this] { return jsonToLua(lua, lightmapsToJson(scene.lighting.lightmaps)); });
    lightmapsApi.set_function("set", [this](sol::this_state L, const sol::object& fields) {
        if (fields.get_type() != sol::type::table)
            raise(L, std::format("kast.lightmaps.set: expected a table, got {}", toString(L, fields)));
        nlohmann::ordered_json json = lightmapsToJson(scene.lighting.lightmaps);
        std::string error;
        for (const auto& [key, value] : fields.as<sol::table>()) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("kast.lightmaps.set: field names must be strings, got {}", toString(L, key)));
            const std::string name = key.as<std::string>();
            if (name != "enabled" && name != "intensity" && name != "directional")
                raise(L, std::format("kast.lightmaps.set: \"{}\" can't be set at run time (only enabled, intensity and "
                                     "directional: the rest need Bake Lightmaps)",
                                     name));
            nlohmann::json plain;
            if (!luaToJson(value, plain, error))
                raise(L, std::format("kast.lightmaps.set: \"{}\": {}", name, error));
            json[name] = plain;
        }
        SceneLightmaps lightmaps;
        if (!lightmapsFromJson(json, lightmaps, error))
            raise(L, "kast.lightmaps.set: " + error);
        scene.lighting.lightmaps = lightmaps;
    });
}

// kast.sdfgi.get() / set{...}: the scene's SDFGI settings (docs/engine/sdfgi.html). Only enabled and energy can be set:
// the others resize or rebuild the cascades, which belongs to the scene, not a script. All-or-nothing, as kast.sky.set.
void LuaSystems::State::bindSdfgiApi()
{
    sdfgiApi = lua.create_table();
    sdfgiApi.set_function("get", [this] { return jsonToLua(lua, sdfgiToJson(scene.lighting.sdfgi)); });
    sdfgiApi.set_function("set", [this](sol::this_state L, const sol::object& fields) {
        if (fields.get_type() != sol::type::table)
            raise(L, std::format("kast.sdfgi.set: expected a table, got {}", toString(L, fields)));
        nlohmann::ordered_json json = sdfgiToJson(scene.lighting.sdfgi);
        std::string error;
        for (const auto& [key, value] : fields.as<sol::table>()) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("kast.sdfgi.set: field names must be strings, got {}", toString(L, key)));
            const std::string name = key.as<std::string>();
            if (name != "enabled" && name != "energy")
                raise(L, std::format("kast.sdfgi.set: \"{}\" can't be set at run time (only enabled and energy)", name));
            nlohmann::json plain;
            if (!luaToJson(value, plain, error))
                raise(L, std::format("kast.sdfgi.set: \"{}\": {}", name, error));
            json[name] = plain;
        }
        SceneSdfgi sdfgi;
        if (!sdfgiFromJson(json, sdfgi, error))
            raise(L, "kast.sdfgi.set: " + error);
        scene.lighting.sdfgi = sdfgi;
    });
}

// kast.<luaName>.get() / set{...}: the scene's "postProcessing" block (docs/engine/post-processing.html), e.g. kast.ssao.
// Every field can be set at run time (none rebuilds a resource); all-or-nothing, as kast.sky.set.
sol::table LuaSystems::State::postBlockApi(const char* luaName, const char* block)
{
    sol::table table = lua.create_table();
    table.set_function("get", [this, block] { return jsonToLua(lua, postBlockToJson(scene.postProcessing, block)); });
    table.set_function("set", [this, luaName, block](sol::this_state L, const sol::object& fields) {
        if (fields.get_type() != sol::type::table)
            raise(L, std::format("kast.{}.set: expected a table, got {}", luaName, toString(L, fields)));
        nlohmann::ordered_json json = postBlockToJson(scene.postProcessing, block);
        std::string error;
        for (const auto& [key, value] : fields.as<sol::table>()) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("kast.{}.set: field names must be strings, got {}", luaName, toString(L, key)));
            const std::string name = key.as<std::string>();
            nlohmann::json plain;
            if (!luaToJson(value, plain, error))
                raise(L, std::format("kast.{}.set: \"{}\": {}", luaName, name, error));
            json[name] = plain; // an unknown field or a bad value: postBlockFromJson says which
        }
        if (!postBlockFromJson(json, block, scene.postProcessing, error))
            raise(L, std::format("kast.{}.set: {}", luaName, error));
    });
    return table;
}

// kast.world.*: see lua_world_api.h.
void LuaSystems::State::bindWorldApi()
{
    worldApi = makeWorldApi(lua, world);
    // kast.world.request(name, data?, handlers?): kast.world.send tagged with a requestId (returned), for a predicted
    // action. handlers {predict, confirmed, rejected, timeout}: predict() runs at once, after the send; then exactly
    // one of confirmed(data) (the server's kast.reply ok) or rejected(reason) (its reason, "timeout" after timeout
    // seconds, default 2, or "disconnected") runs from the event drain, as the calling system's.
    worldApi.set_function("request", [this](sol::this_state L, const sol::object& name, const sol::object& data,
                                            const sol::object& handlers) {
        if (name.get_type() != sol::type::string)
            raise(L, std::format("kast.world.request: name must be a string, got {}", toString(L, name)));
        const nlohmann::json json = worldEventData(L, "kast.world.request", data);
        sol::table h = handlers.get_type() == sol::type::table ? handlers.as<sol::table>() : lua.create_table();
        if (handlers.get_type() != sol::type::table && handlers.get_type() != sol::type::lua_nil)
            raise(L, std::format("kast.world.request: handlers must be a table, got {}", toString(L, handlers)));
        const auto handler = [&](const char* key) {
            const sol::object fn = h[key];
            if (fn.get_type() != sol::type::function && fn.get_type() != sol::type::lua_nil)
                raise(L, std::format("kast.world.request: handlers.{} must be a function, got {}", key, toString(L, fn)));
            // on the main thread: fn's own may be a coroutine, collected before the drain
            return fn.get_type() == sol::type::function ? sol::protected_function(lua.lua_state(), fn)
                                                        : sol::protected_function();
        };
        const sol::object timeout = h["timeout"];
        const double seconds = timeout.get_type() == sol::type::number ? timeout.as<double>()
                               : timeout.get_type() == sol::type::lua_nil ? 2.0 : -1.0;
        if (!(seconds > 0.0 && seconds <= 3600.0))
            raise(L, std::format("kast.world.request: handlers.timeout must be seconds in (0, 3600], got {}",
                                 toString(L, timeout)));
        if (!current)
            raise(L, "kast.world.request: only a system can make requests");
        Request request{current, handler("confirmed"), handler("rejected")};
        const sol::protected_function predict = handler("predict");
        uint32_t id = 0;
        try {
            id = world.request(name.as<std::string>(), json,
                               std::chrono::milliseconds(std::max<int64_t>(1, std::llround(seconds * 1000.0))));
        } catch (const std::logic_error& e) { // WorldClient's messages start with the function's name
            raise(L, std::format("kast.world.{}", e.what()));
        }
        requests[id] = std::move(request);
        if (replies.id() == 0)
            replies = bus.subscribe("world.reply", [this](const nlohmann::json& reply) {
                const auto it = requests.find(reply.value("requestId", 0u));
                if (it == requests.end()) // an earlier session's
                    return;
                Request r = std::move(it->second);
                requests.erase(it);
                const bool ok = reply.value("ok", false);
                const sol::protected_function& fn = ok ? r.confirmed : r.rejected;
                if (fn && r.owner->ok)
                    call(*r.owner, fn, ok ? "request confirmed" : "request rejected",
                         ok ? jsonToLua(lua, reply.value("data", nlohmann::json::object()))
                            : sol::make_object(lua, reply.value("reason", std::string())));
            });
        if (predict) { // inside the caller's own call: its error, at this line
            sol::protected_function_result result = predict();
            if (!result.valid())
                raise(L, std::format("kast.world.request: predict: {}", result.get<sol::error>().what()));
        }
        return id;
    });
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
    chat_.reset(auth);
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
                                     world_, chat_, gui_, display_);
    state_->playOnStart(); // before the inits, which may stop them
    std::vector<std::string> systems; // every loaded scene's, in load order, each once
    for (const LoadedScene& loaded : scene.loadedScenes())
        for (const std::string& system : scene.settingsOf(loaded.slot).systems)
            if (std::ranges::find(systems, system) == systems.end())
                systems.push_back(system);
    state_->init(systems);
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
        if (!auth_.handle(event) && !world_.handle(event) && !chat_.handle(event))
            bus_.publish(std::move(event.name), std::move(event.payload), "kast.net");
    for (NetEvent& event : auth_.poll()) {
        world_.observe(event); // realm lists, for kast.world.connect
        bus_.publish(std::move(event.name), std::move(event.payload), "kast.auth");
    }
    for (NetEvent& event : world_.poll()) {
        chat_.observe(event); // where the chat service is, and when the world connection ends
        bus_.publish(std::move(event.name), std::move(event.payload), "kast.world");
    }
    for (NetEvent& event : chat_.poll())
        bus_.publish(std::move(event.name), std::move(event.payload), "kast.chat");
    if (state_->gui)
        state_->gui->advance(dt); // floating world texts age (kast.gui.worldText)
    state_->tick(dt);
    replication_.update(dt); // sends what this frame's systems did to the local player
    if (state_->sceneRequests.empty())
        return false;
    std::vector<SceneRequest> requests = std::move(state_->sceneRequests);
    state_->sceneRequests.clear();
    bool switched = false;
    for (SceneRequest& request : requests) {
        if (request.kind == SceneRequest::Kind::Switch) { // only ever the first
            Scene& scene = state_->scene;
            const std::optional<std::filesystem::path> projectRoot = state_->projectRoot;
            Input* input = state_->input;
            if (input)
                input->clear(); // held buttons and the cursor lock belong to the old scene
            endSession(); // old systems' shutdowns see the old scene; the Lua state and queued events are discarded
            scene = std::move(request.scene);
            log::info(std::format("Switched to scene '{}'", request.path));
            launch(scene, projectRoot, request.path, input); // C++ subscriptions, the bus history and game states carry over
            switched = true;
        } else if (request.kind == SceneRequest::Kind::Add) {
            state_->addScene(request);
        } else if (request.kind == SceneRequest::Kind::Unload) {
            state_->unloadScene(request.path);
        } else {
            state_->scene.setActiveScene(state_->slotOf(request.path));
            log::info(std::format("Active scene '{}'", request.path));
        }
    }
    ++sceneChanges_;
    return switched;
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
    chat_.reset();
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

std::string LuaSystems::scenePath() const
{
    return state_ ? state_->scenePaths.at(state_->scene.activeSlot()) : std::string();
}

} // namespace kast
