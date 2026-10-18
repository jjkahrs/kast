#include "runtime.h"
#include "../core/log.h"
#include "../world/components.h"
#include "lua_state_machines.h"
#include "npcs.h"
#include "sandbox.h"

#include <messages.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace kast::lua {
namespace {

// Raises a Lua error at the calling script's file:line. sol2 turns the exception into lua_error.
[[noreturn]] void raise(lua_State* L, const std::string& message) {
    luaL_where(L, 1);
    std::string where = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error(where + message);
}

// The read-only built-ins (Character on players, Npc on NPCs, Spawn on spawn-file NPCs, Transform on both; Transform is
// writable on NPCs), plus the Lua component types.
enum class Builtin { none, character, transform, npc, spawn };
struct Type {
    Builtin builtin = Builtin::none;
    const ComponentDef* def = nullptr;
    std::string_view name() const {
        if (def) return def->name;
        switch (builtin) {
        case Builtin::character: return "Character";
        case Builtin::npc: return "Npc";
        case Builtin::spawn: return "Spawn";
        default: return "Transform";
        }
    }
};

constexpr size_t kMaxNpcNameBytes = 255; // for name and template, each

struct System {
    std::string path; // "systems/regen.lua"
    std::filesystem::path file;
    sol::environment env;
    sol::protected_function update, shutdown, onEnter, onLeave;
    std::vector<EventBus::Subscription> subscriptions; // kast.subscribe; cleared when the system is disabled
    bool ok = false;
    std::string error;
};

uint32_t wire(entt::entity e) { return entt::to_integral(e); }

// The writable StateMachine built-in, stored like a Lua component that is never persisted or replicated: kast.add/set/
// get/has/remove/query and their checks work on it unchanged, and World (persistence, replication) never sees it.
Defs with_state_machine(Defs defs) {
    auto def = std::make_shared<ComponentDef>();
    def->name = def->displayName = kStateMachineComponent;
    def->path = "(built-in)";
    for (const char* name : {"machine", "initialState"}) {
        FieldDesc& field = def->fields.emplace_back();
        field.name = name;
        field.kind = FieldKind::String;
        field.defaultValue = "";
    }
    defs.push_back(std::move(def));
    return defs;
}

} // namespace

struct Runtime::Impl {
    entt::registry& reg;
    Defs defs;
    NpcHooks npcs;
    sol::state lua;
    sol::table api;
    sol::table fsmApi;                     // kast.fsm.*
    sol::table mapApi;                     // kast.map.*
    EventBus bus;                          // after lua: Lua subscribers hold functions of that state
    StateMachines fsm{bus, reg};           // after lua: its definitions hold Lua callbacks
    std::vector<System> systems;           // after bus: their subscriptions unsubscribe from it
    std::vector<ClientEvent> clientEvents; // kast.sendTo/sendNearby/broadcast since the last take
    NpcDefs npcDefs;                       // npcs/, loaded after the machines (StateMachine data names one)
    System* current = nullptr;
    std::string callbackPath; // the definition file whose enter/exit is running, if no system is

    Impl(entt::registry& r, Defs d, NpcHooks n) : reg(r), defs(with_state_machine(std::move(d))), npcs(std::move(n)) {
        open_sandbox(lua);
        bind_api();
        bind_fsm_api();
        bind_map_api();
    }

    std::string who() const { return current ? current->path : callbackPath.empty() ? std::string("lua") : callbackPath; }

    // Runs fn; returns the error ("path:line: message"), empty on success.
    template <class... Args>
    std::string call(System& system, const sol::protected_function& fn, std::string_view what, Args&&... args) {
        current = &system;
        std::string error;
        try {
            const ScriptBudget budget(lua.lua_state());
            sol::protected_function_result result = fn(std::forward<Args>(args)...);
            if (!result.valid()) error = result.get<sol::error>().what();
        } catch (const std::exception& e) { // sol2 conversions; nothing may escape to the tick
            error = std::format("{}: {}: {}", system.path, what, e.what());
        }
        current = nullptr;
        return error;
    }

    // A callback after startup: an error disables the system, the server and the other systems keep running.
    template <class... Args>
    void run(System& system, const sol::protected_function& fn, std::string_view what, Args&&... args) {
        if (!system.ok || !fn.valid()) return;
        std::string error = call(system, fn, what, std::forward<Args>(args)...);
        if (error.empty()) return;
        disable(system, std::move(error));
        log::error("Lua system disabled", {{"system", system.path}, {"callback", what}, {"error", system.error}});
    }

    void disable(System& system, std::string error) {
        system.ok = false;
        system.error = std::move(error);
        system.subscriptions.clear();
    }

    std::string load(System& system) {
        std::string error;
        system.env = environment();
        sol::protected_function body = load_sandboxed(lua, system.file, "@" + system.path, system.env, error);
        if (!body.valid()) return error.starts_with("cannot open") ? std::format("{}: {}", system.path, error) : error;
        current = &system;
        sol::protected_function_result result = [&] {
            const ScriptBudget budget(lua.lua_state());
            return body();
        }();
        current = nullptr;
        if (!result.valid()) return result.get<sol::error>().what();
        if (result.get_type() != sol::type::table)
            return std::format("{}: must return a table with an update function", system.path);
        sol::table table = result;
        sol::object update = table["update"];
        if (update.get_type() != sol::type::function)
            return std::format("{}: returned table has no update function", system.path);
        system.update = update;
        for (auto [name, slot] : {std::pair{"init", static_cast<sol::protected_function*>(nullptr)},
                                  std::pair{"shutdown", &system.shutdown}, std::pair{"onEnter", &system.onEnter},
                                  std::pair{"onLeave", &system.onLeave}}) {
            sol::object fn = table[name];
            if (fn.get_type() == sol::type::lua_nil) continue;
            if (fn.get_type() != sol::type::function) return std::format("{}: \"{}\" must be a function", system.path, name);
            if (slot) *slot = fn;
        }
        system.ok = true;
        if (sol::object init = table["init"]; init.get_type() == sol::type::function)
            if (std::string e = call(system, init, "init"); !e.empty()) {
                disable(system, e);
                return e;
            }
        return {};
    }

    sol::table read_only(const sol::table& source) {
        // A fresh copy behind a proxy: writes raise an error, and even rawset only changes this system's copy.
        sol::table copy = lua.create_table();
        for (const auto& [key, value] : source) copy[key] = value;
        sol::table meta = lua.create_table();
        meta["__index"] = copy;
        meta["__newindex"] = [](sol::this_state L, sol::object, sol::object, sol::object) { raise(L, "kast is read-only"); };
        meta["__metatable"] = false;
        sol::table proxy = lua.create_table();
        proxy[sol::metatable_key] = meta;
        return proxy;
    }

    sol::environment environment() {
        sol::environment env = sandbox_environment(lua);
        sol::table kast = lua.create_table();
        for (const auto& [key, value] : api) kast[key] = value;
        kast["fsm"] = read_only(fsmApi);
        kast["map"] = read_only(mapApi);
        env["kast"] = read_only(kast);
        return env;
    }

    // Loads the definitions (callbacks bound to this state) and lists the game machines; every error.
    std::vector<std::string> load_machines(const std::filesystem::path& root, std::vector<std::string> gameMachines) {
        StateMachineDefinitions loaded = parse_state_machine_definitions(
            lua, root, [this] { return environment(); },
            [this](const sol::protected_function& fn, const std::string& path, const std::string& what) {
                return bind_callback(fn, path, what);
            });
        for (std::string& e : fsm.setup(std::move(loaded.valid), std::move(gameMachines)))
            loaded.errors.push_back(std::move(e));
        return std::move(loaded.errors);
    }

    bool machine_exists(const std::string& name) const { return fsm.definition(name) != nullptr; }

    std::vector<std::string> load_npcs(const std::filesystem::path& root) {
        NpcDefsResult loaded = load_npc_defs(lua, root, defs, [this](const std::string& n) { return machine_exists(n); });
        npcDefs = std::move(loaded.defs);
        return std::move(loaded.errors);
    }

    // An enter/exit callback: an error (with its traceback) is logged, and no system is disabled.
    StateCallback bind_callback(const sol::protected_function& fn, const std::string& path, const std::string& what) {
        return [this, callback = fn, path, what](const Transition& t) {
            sol::table ctx = lua.create_table();
            ctx["machine"] = t.machine;
            if (t.entity) ctx["entity"] = *t.entity;
            if (t.from) ctx["from"] = *t.from;
            ctx["to"] = t.to;
            System* caller = std::exchange(current, nullptr); // a system's kast.fsm.change may have called this
            std::string callerPath = std::exchange(callbackPath, path);
            std::string error;
            try {
                const ScriptBudget budget(lua.lua_state());
                sol::protected_function_result result = callback(ctx);
                if (!result.valid()) error = result.get<sol::error>().what();
            } catch (const std::exception& e) { // sol2 conversions; nothing may escape to the tick
                error = std::format("{}: {}", path, e.what());
            }
            current = caller;
            callbackPath = std::move(callerPath);
            if (!error.empty()) log::error(std::format("State machine {}: {} failed: {}", path, what, error));
        };
    }

    // kast.fsm.*: thin wrappers over StateMachines, whose std::invalid_argument (a bad target) becomes a Lua error at
    // the calling line. Copied from the engine's bindFsmApi.
    void bind_fsm_api() {
        using Target = StateMachines::Target;
        const auto target = [](lua_State* L, const char* function, const sol::object& value) -> Target {
            if (value.get_type() == sol::type::string) return value.as<std::string>();
            if (value.get_type() == sol::type::number) {
                const double id = value.as<double>();
                if (id >= 0.0 && id <= 4294967295.0 && id == std::floor(id)) return static_cast<EntityId>(id);
                raise(L, std::format("{}: invalid entity id {}", function, to_string(L, value)));
            }
            raise(L, std::format("{}: target must be a state machine name or an entity id, got {}", function,
                                 to_string(L, value)));
        };
        const auto stateName = [](lua_State* L, const char* function, const sol::object& value) {
            if (value.get_type() != sol::type::string)
                raise(L, std::format("{}: state must be a string, got {}", function, to_string(L, value)));
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

    // Entities are server wire ids: player characters and NPCs.
    static bool in_world(const entt::registry& reg, entt::entity e) {
        return reg.valid(e) && reg.any_of<world::CharacterRef, world::Npc>(e);
    }

    entt::entity entity(lua_State* L, const sol::object& id, const char* function = nullptr) {
        if (id.get_type() == sol::type::number) {
            const double value = id.as<double>();
            if (value >= 0.0 && value <= 4294967295.0 && value == std::floor(value)) {
                const auto e = entt::entity{static_cast<uint32_t>(value)};
                if (in_world(reg, e)) return e;
            }
        }
        raise(L, std::format("{}invalid entity id {}", function ? std::string(function) + ": " : "", to_string(L, id)));
    }

    // A client event's target: any wire id; one not in the world is skipped when the events are sent.
    uint32_t target(lua_State* L, const char* function, const sol::object& id) {
        if (id.get_type() == sol::type::number) {
            const double value = id.as<double>();
            if (value >= 0.0 && value <= 4294967295.0 && value == std::floor(value)) return static_cast<uint32_t>(value);
        }
        raise(L, std::format("{}: invalid entity id {}", function, to_string(L, id)));
    }

    std::string event_name(lua_State* L, const char* function, const sol::object& name) {
        if (name.get_type() != sol::type::string || name.as<std::string>().empty())
            raise(L, std::format("{}: event name must be a non-empty string, got {}", function, to_string(L, name)));
        return name.as<std::string>();
    }

    nlohmann::json payload(lua_State* L, const char* function, const sol::object& value) {
        nlohmann::json json;
        if (std::string error; !lua_to_json(value, json, error)) raise(L, std::format("{}: {}", function, error));
        return json;
    }

    void queue_client_event(lua_State* L, const char* function, ClientEvent::To to, uint32_t entity,
                            const sol::object& name, const sol::object& value) {
        std::string event = event_name(L, function, name);
        // replace: a Lua string may hold bytes that aren't UTF-8, which JSON can't carry
        std::string json = payload(L, function, value).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!proto::ServerEvent::fits(event.size(), json.size()))
            raise(L, std::format("{}: event \"{}\" is too large to send: {} bytes of name and JSON payload, at most {}",
                                 function, event, event.size() + json.size(), proto::kMaxPayloadBytes - 4));
        clientEvents.push_back({to, entity, std::move(event), std::move(json)});
    }

    Type type(lua_State* L, const std::string& name) {
        if (name == "Character") return {Builtin::character, nullptr};
        if (name == "Transform") return {Builtin::transform, nullptr};
        if (name == "Npc") return {Builtin::npc, nullptr};
        if (name == "Spawn") return {Builtin::spawn, nullptr};
        for (const auto& def : defs)
            if (def->name == name) return {Builtin::none, def.get()};
        raise(L, std::format("unknown component \"{}\"", name));
    }

    bool has(entt::entity e, const Type& t) {
        switch (t.builtin) {
        case Builtin::character: return reg.all_of<world::CharacterRef>(e);
        case Builtin::npc: return reg.all_of<world::Npc>(e);
        case Builtin::spawn: return reg.all_of<world::SpawnRef>(e);
        case Builtin::transform: return true;
        case Builtin::none: break;
        }
        return pool(reg, t.def->name).contains(e);
    }

    const ComponentDef& writable(lua_State* L, const Type& t) {
        if (!t.def) raise(L, std::format("\"{}\" is read-only on the server", t.name()));
        return *t.def;
    }

    void changed(entt::entity e, const ComponentDef& def) {
        // replication diffs on its own; NPCs are never saved
        if (def.persist && reg.all_of<world::CharacterRef>(e)) reg.emplace_or_replace<world::Dirty>(e);
        if (def.name == kStateMachineComponent) fsm.syncEntity(e); // a new "machine" enters its initial state now
    }

    // A number that is finite as the wire's f32, or raises "<prefix>"<name>" must be a finite number".
    static float finite(lua_State* L, std::string_view prefix, std::string_view name, const sol::object& value) {
        const double number = value.get_type() == sol::type::number ? value.as<double>() : NAN;
        if (!std::isfinite(number) || std::fabs(number) > std::numeric_limits<float>::max())
            raise(L, std::format("{}\"{}\" must be a finite number", prefix, name));
        return static_cast<float>(number);
    }

    // {x=, y=, z=}, each finite as an f32, or raises "<prefix>"<name>" must be a table ...".
    static proto::Vec3 position(lua_State* L, std::string_view prefix, const sol::object& value,
                                std::string_view name = "position") {
        const auto bad = [&] {
            raise(L, std::format("{}\"{}\" must be a table {{x=, y=, z=}} of finite numbers", prefix, name));
        };
        if (value.get_type() != sol::type::table) bad();
        const sol::table table = value.as<sol::table>();
        float xyz[3];
        for (int i = 0; i < 3; ++i) {
            const sol::object c = table[std::string(1, "xyz"[i])];
            const double number = c.get_type() == sol::type::number ? c.as<double>() : NAN;
            if (!std::isfinite(number) || std::fabs(number) > std::numeric_limits<float>::max()) bad();
            xyz[i] = static_cast<float>(number);
        }
        return {xyz[0], xyz[1], xyz[2]};
    }

    // A loaded map's id, or raises "<prefix>"map" must be ...".
    uint16_t map_id(lua_State* L, std::string_view prefix, const sol::object& value) {
        const double number = value.get_type() == sol::type::number ? value.as<double>() : NAN;
        if (!(number >= 0 && number <= 65535 && number == std::floor(number)) || !npcs.map(static_cast<uint16_t>(number)))
            raise(L, std::format("{}\"map\" must be the id of a loaded map, got {}", prefix, to_string(L, value)));
        return static_cast<uint16_t>(number);
    }

    // kast.set(npc, "Transform", {position=, heading=}): partial and all-or-nothing like any set. The position is
    // clamped onto the NPC's map, on its ground (y is ignored); script moves are trusted (no speed check) and replicate as EntityMove like an
    // accepted player move. The map is read-only: kast.transfer changes it.
    void set_npc_transform(lua_State* L, entt::entity e, const sol::table& fields) {
        auto& tf = reg.get<world::Transform>(e);
        proto::Vec3 pos = tf.pos;
        float heading = tf.heading;
        for (const auto& [key, value] : fields) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("\"Transform\": field names must be strings, got {}", to_string(L, key)));
            const std::string field = key.as<std::string>();
            if (field == "position") pos = position(L, "\"Transform\": ", value);
            else if (field == "heading") heading = finite(L, "\"Transform\": ", "heading", value);
            else if (field == "map") raise(L, "\"Transform\": \"map\" is read-only; use kast.transfer");
            else raise(L, std::format("\"Transform\": unknown field \"{}\"", field));
        }
        pos = npcs.clamp(reg.get<world::MapRef>(e).map, pos);
        if (pos == tf.pos && heading == tf.heading) return;
        tf.pos = pos;
        tf.heading = heading;
        reg.emplace_or_replace<world::Moved>(e);
    }

    // kast.create(name, {npc= | template=, components=?, map=?, position=?, heading=?}): every argument is checked
    // before anything is created.
    uint32_t create_npc(lua_State* L, const sol::object& name, const sol::object& opts) {
        const auto text = [&](std::string_view what, const sol::object& value) {
            if (value.get_type() != sol::type::string || value.as<std::string>().empty() ||
                value.as<std::string>().size() > kMaxNpcNameBytes)
                raise(L, std::format("kast.create: {} must be a non-empty string of at most {} bytes, got {}", what,
                                     kMaxNpcNameBytes, to_string(L, value)));
            return value.as<std::string>();
        };
        std::string npcName = text("name", name);
        if (opts.get_type() != sol::type::table)
            raise(L, std::format("kast.create: opts must be a table {{npc = ...}} or {{template = ...}}, got {}",
                                 to_string(L, opts)));
        const sol::table table = opts.as<sol::table>();
        for (const auto& [key, unused] : table) {
            const std::string k = key.get_type() == sol::type::string ? key.as<std::string>() : "";
            if (k != "npc" && k != "template" && k != "components" && k != "map" && k != "position" && k != "heading")
                raise(L, std::format("kast.create: unknown option {}", to_string(L, key)));
        }
        const sol::object npcOpt = table["npc"], templateOpt = table["template"];
        if ((npcOpt.get_type() == sol::type::lua_nil) == (templateOpt.get_type() == sol::type::lua_nil))
            raise(L, "kast.create: opts needs exactly one of npc (an npcs/ definition) and template");
        const NpcDef* def = nullptr;
        if (npcOpt.get_type() != sol::type::lua_nil) {
            const std::string wanted = text("npc", npcOpt);
            const auto it = npcDefs.find(wanted);
            if (it == npcDefs.end())
                raise(L, std::format("kast.create: unknown npc \"{}\" (no definition in {}/)", wanted, kNpcsDir));
            def = &it->second;
        }
        std::string templateName = def ? def->templateName : text("template", templateOpt);
        std::string error;
        std::optional<Json> components =
            merge_components(table["components"], def ? def->components : Json::object(), defs,
                             [this](const std::string& n) { return machine_exists(n); }, error);
        if (!components) raise(L, "kast.create: " + error);
        std::optional<uint16_t> map;
        if (const sol::object m = table["map"]; m.get_type() != sol::type::lua_nil) map = map_id(L, "kast.create: ", m);
        std::optional<proto::Vec3> pos;
        if (const sol::object p = table["position"]; p.get_type() != sol::type::lua_nil)
            pos = position(L, "kast.create: ", p);
        float heading = 0;
        if (const sol::object h = table["heading"]; h.get_type() != sol::type::lua_nil)
            heading = finite(L, "kast.create: ", "heading", h);
        if (reg.view<world::Npc>().size() >= npcs.max)
            raise(L, std::format("kast.create: too many NPCs (MAX_NPCS is {})", npcs.max));
        return wire(npcs.create(std::move(npcName), std::move(templateName), map, pos, heading, def ? def->name : "",
                                *components));
    }

    // kast.transfer(e, map, position, heading?): a player or NPC to a point on a declared tile of a loaded map (the same
    // map too). heading defaults to the current one. false: this process does not own the destination (logged).
    bool transfer(lua_State* L, const sol::object& id, const sol::object& mapValue, const sol::object& posValue,
                  const sol::object& headingValue) {
        const entt::entity e = entity(L, id, "kast.transfer");
        const uint16_t map = map_id(L, "kast.transfer: ", mapValue);
        const proto::Vec3 pos = position(L, "kast.transfer: ", posValue);
        if (!npcs.map(map)->contains(pos.x, pos.z))
            raise(L, std::format("kast.transfer: position ({}, {}) is not on a declared tile of map {}", pos.x, pos.z, map));
        const float heading = headingValue.get_type() == sol::type::lua_nil
                                  ? reg.get<world::Transform>(e).heading
                                  : finite(L, "kast.transfer: ", "heading", headingValue);
        return npcs.transfer(e, map, pos, heading);
    }

    // kast.map.area(e) / kast.map.areaAt(map, x, y, z): {id, name, flags, music} of the highest-priority area there
    // (flags include the parent's), or nil. Read-only: what an area's flags mean is up to the game's Lua.
    sol::object area_table(uint16_t map, float x, float y, float z) {
        const auto& areas = npcs.map(map)->areas;
        const int16_t i = map::area_at(areas, x, y, z);
        if (i < 0) return sol::make_object(lua, sol::lua_nil);
        const map::Area& a = areas.list[i];
        sol::table t = lua.create_table();
        t["id"] = a.id;
        t["name"] = a.name;
        t["flags"] = json_to_lua(lua, a.flags);
        if (!a.music.empty()) t["music"] = a.music;
        return t;
    }

    void bind_map_api() {
        mapApi = lua.create_table();
        mapApi.set_function("area", [this](sol::this_state L, const sol::object& id) {
            const entt::entity e = entity(L, id, "kast.map.area");
            const auto& pos = reg.get<world::Transform>(e).pos;
            return area_table(reg.get<world::MapRef>(e).map, pos.x, pos.y, pos.z);
        });
        mapApi.set_function("areaAt", [this](sol::this_state L, const sol::object& map, const sol::object& x,
                                             const sol::object& y, const sol::object& z) {
            const uint16_t id = map_id(L, "kast.map.areaAt: ", map);
            return area_table(id, finite(L, "kast.map.areaAt: ", "x", x), finite(L, "kast.map.areaAt: ", "y", y),
                              finite(L, "kast.map.areaAt: ", "z", z));
        });
        // kast.map.spawn(map): {position = {x, y, z}, heading} of the map's spawn point (map.json "spawn").
        mapApi.set_function("spawn", [this](sol::this_state L, const sol::object& map) {
            const map::MapInfo* info = npcs.map(map_id(L, "kast.map.spawn: ", map));
            return lua.create_table_with(
                "position", lua.create_table_with("x", info->spawn[0], "y", info->spawn[1], "z", info->spawn[2]),
                "heading", info->spawnHeading);
        });
        // kast.map.ground(map, x, z): the ground's height there, or nil off the map's tiles.
        mapApi.set_function("ground", [this](sol::this_state L, const sol::object& map, const sol::object& x,
                                             const sol::object& z) {
            const uint16_t id = map_id(L, "kast.map.ground: ", map);
            const auto y = npcs.ground(id, finite(L, "kast.map.ground: ", "x", x), finite(L, "kast.map.ground: ", "z", z));
            return y ? sol::make_object(lua, *y) : sol::make_object(lua, sol::lua_nil);
        });
        // kast.map.path(map, from, to): the navmesh path from `from` to `to` ({x=, y=, z=} each) as a list of points,
        // `from` snapped onto the mesh first and `to` last, or nil (no navmesh on the map, an end not near it, or no way).
        mapApi.set_function("path", [this](sol::this_state L, const sol::object& map, const sol::object& from,
                                           const sol::object& to) {
            const uint16_t id = map_id(L, "kast.map.path: ", map);
            const auto points = npcs.path(id, position(L, "kast.map.path: ", from, "from"),
                                          position(L, "kast.map.path: ", to, "to"));
            if (points.empty()) return sol::make_object(lua, sol::lua_nil);
            sol::table list = lua.create_table(int(points.size()), 0);
            for (const auto& p : points) list.add(lua.create_table_with("x", p.x, "y", p.y, "z", p.z));
            return sol::make_object(lua, list);
        });
    }

    void bind_api() {
        api = lua.create_table();

        // Matches are captured when query is called, in wire id order: entities gone later in the loop are skipped.
        api.set_function("query", [this](sol::this_state L, sol::variadic_args names) {
            std::vector<Type> types;
            for (const sol::object name : names) {
                if (name.get_type() != sol::type::string)
                    raise(L, std::format("component name must be a string, got {}", to_string(L, name)));
                types.push_back(type(L, name.as<std::string>()));
            }
            std::vector<uint32_t> matches;
            for (auto e : reg.view<world::Transform>()) // every player and NPC
                if (std::ranges::all_of(types, [&](const Type& t) { return has(e, t); })) matches.push_back(wire(e));
            std::ranges::sort(matches);
            // nil ends the generic for.
            return std::function<sol::object()>([this, matches = std::move(matches), next = size_t{0}]() mutable {
                while (next < matches.size()) {
                    const uint32_t id = matches[next++];
                    if (in_world(reg, entt::entity{id}))
                        return sol::make_object(lua, id);
                }
                return sol::make_object(lua, sol::lua_nil);
            });
        });
        api.set_function("create", [this](sol::this_state L, const sol::object& name, const sol::object& opts) {
            return create_npc(L, name, opts);
        });
        api.set_function("transfer", [this](sol::this_state L, const sol::object& id, const sol::object& map,
                                            const sol::object& pos, const sol::object& heading) {
            return transfer(L, id, map, pos, heading);
        });
        // kast.setMovable(e, movable): false refuses every Move of the player (Correction back to where it stands)
        // until set true again. Not saved: a character logs in movable.
        api.set_function("setMovable", [this](sol::this_state L, const sol::object& id, const sol::object& movable) {
            const entt::entity e = entity(L, id, "kast.setMovable");
            if (!reg.all_of<world::CharacterRef>(e))
                raise(L, std::format("kast.setMovable: entity {} is an NPC; only players send moves", to_string(L, id)));
            if (movable.get_type() != sol::type::boolean)
                raise(L, std::format("kast.setMovable: movable must be a boolean, got {}", to_string(L, movable)));
            if (movable.as<bool>()) reg.remove<world::MoveLock>(e);
            else reg.emplace_or_replace<world::MoveLock>(e);
        });
        // Players leave only by logging out: destroy takes NPCs.
        api.set_function("destroy", [this](sol::this_state L, const sol::object& id) {
            const entt::entity e = entity(L, id, "kast.destroy");
            if (!reg.all_of<world::Npc>(e))
                raise(L, std::format("kast.destroy: entity {} is a player; only NPCs can be destroyed", to_string(L, id)));
            npcs.destroy(e);
        });
        api.set_function("has", [this](sol::this_state L, const sol::object& id, const std::string& name) {
            return has(entity(L, id), type(L, name));
        });
        api.set_function("add", [this](sol::this_state L, const sol::object& id, const std::string& name) {
            const entt::entity e = entity(L, id);
            const ComponentDef& def = writable(L, type(L, name));
            auto& p = pool(reg, def.name);
            if (p.contains(e)) return;
            p.emplace(e, ComponentData{default_component(def)});
            changed(e, def);
        });
        api.set_function("remove", [this](sol::this_state L, const sol::object& id, const std::string& name) {
            const entt::entity e = entity(L, id);
            const Type t = type(L, name);
            if (!t.def) raise(L, std::format("\"{}\" cannot be removed", name));
            auto& p = pool(reg, t.def->name);
            if (!p.contains(e)) return;
            p.remove(e);
            changed(e, *t.def);
        });
        api.set_function("get", [this](sol::this_state L, const sol::object& id, const std::string& name) {
            const entt::entity e = entity(L, id);
            const Type t = type(L, name);
            if (!has(e, t)) raise(L, std::format("entity {} has no \"{}\"", to_string(L, id), name));
            sol::table table = lua.create_table();
            if (t.builtin == Builtin::character) {
                const auto& ch = reg.get<world::CharacterRef>(e);
                table["charId"] = static_cast<double>(ch.charId);
                table["name"] = ch.name;
            } else if (t.builtin == Builtin::npc) {
                const auto& npc = reg.get<world::Npc>(e);
                table["name"] = npc.name;
                table["template"] = npc.templateName;
                table["npc"] = npc.npc;
            } else if (t.builtin == Builtin::spawn) {
                const map::Spawn& s = *npcs.spawn(reg.get<world::SpawnRef>(e).record);
                table["id"] = s.id;
                table["pos"] = lua.create_table_with("x", s.pos[0], "y", s.pos[1], "z", s.pos[2]);
                table["heading"] = s.heading;
                if (s.path) {
                    sol::table points = lua.create_table();
                    for (const auto& p : s.path->points) points.add(lua.create_table_with("x", p[0], "y", p[1], "z", p[2]));
                    table["path"] = points;
                    table["wait"] = s.path->wait;
                    table["loop"] = s.path->loop;
                }
            } else if (t.builtin == Builtin::transform) {
                const auto& tf = reg.get<world::Transform>(e);
                table["position"] = lua.create_table_with("x", tf.pos.x, "y", tf.pos.y, "z", tf.pos.z);
                table["heading"] = tf.heading;
                table["map"] = reg.get<world::MapRef>(e).map;
            } else {
                const Json& json = pool(reg, t.def->name).get(e).value;
                for (const FieldDesc& field : t.def->fields) table[field.name] = field_json_to_lua(lua, field, json[field.name]);
            }
            return table;
        });
        // Partial tables: listed fields change, the rest keep their values. All-or-nothing: any bad field, or a value
        // min/max / enum values reject, leaves the component unchanged.
        // ponytail: goes through JSON to reuse the definition checks; convert directly if it shows in profiles.
        api.set_function("set", [this](sol::this_state L, const sol::object& id, const std::string& name,
                                       const sol::table& fields) {
            const entt::entity e = entity(L, id);
            const Type t = type(L, name);
            if (!has(e, t)) raise(L, std::format("entity {} has no \"{}\"", to_string(L, id), name));
            if (t.builtin == Builtin::transform && reg.all_of<world::Npc>(e)) return set_npc_transform(L, e, fields);
            const ComponentDef& def = writable(L, t);
            auto& p = pool(reg, def.name);
            std::string error;
            std::optional<Json> valid = set_fields(def, p.get(e).value, fields, error);
            if (!valid) raise(L, error);
            if (*valid == p.get(e).value) return;
            p.get(e).value = std::move(*valid);
            changed(e, def);
        });

        // Events: queued, delivered by drain_events() after every update (docs/services/world/events.html). Subscriptions belong to
        // the calling system; a handle is a light userdata holding the subscription id (no metatable a script could
        // edit).
        api.set_function("publish", [this](sol::this_state L, const sol::object& name, const sol::object& value) {
            std::string event = event_name(L, "kast.publish", name);
            bus.publish(std::move(event), payload(L, "kast.publish", value), who());
        });
        api.set_function("subscribe", [this](sol::this_state L, const sol::object& name, const sol::object& fn) {
            std::string event = event_name(L, "kast.subscribe", name);
            if (fn.get_type() != sol::type::function)
                raise(L, std::format("kast.subscribe: callback must be a function, got {}", to_string(L, fn)));
            if (!current) raise(L, "kast.subscribe: only a system can subscribe");
            System* owner = current; // systems never move: load() reserved them
            std::string what = std::format("event '{}'", event);
            EventBus::Subscription subscription = bus.subscribe(
                std::move(event), [this, owner, what = std::move(what),
                                   // on the main thread: fn's own may be a coroutine, collected before the drain
                                   callback = sol::protected_function(lua.lua_state(), fn)](const nlohmann::json& p) {
                    run(*owner, callback, what, json_to_lua(lua, p)); // skipped once the owner is disabled
                });
            const uint64_t id = subscription.id();
            owner->subscriptions.push_back(std::move(subscription));
            return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); // ponytail: 64-bit ids in a pointer, x64 only
        });
        api.set_function("unsubscribe", [this](sol::this_state L, const sol::object& handle) {
            if (handle.get_type() != sol::type::lightuserdata)
                raise(L, std::format("kast.unsubscribe: expected a handle from kast.subscribe, got {}", to_string(L, handle)));
            const uint64_t id = reinterpret_cast<uintptr_t>(handle.as<void*>());
            // Another system's handle, or one already unsubscribed, isn't in current's list: nothing happens.
            if (current)
                std::erase_if(current->subscriptions, [id](const EventBus::Subscription& s) { return s.id() == id; });
        });

        // Client events: queued, sent as ServerEvent after component replication (docs/services/world/events.html).
        api.set_function("sendTo", [this](sol::this_state L, const sol::object& id, const sol::object& name,
                                          const sol::object& value) {
            queue_client_event(L, "kast.sendTo", ClientEvent::To::owner, target(L, "kast.sendTo", id), name, value);
        });
        api.set_function("sendNearby", [this](sol::this_state L, const sol::object& id, const sol::object& name,
                                              const sol::object& value) {
            queue_client_event(L, "kast.sendNearby", ClientEvent::To::nearby, target(L, "kast.sendNearby", id), name,
                               value);
        });
        api.set_function("broadcast", [this](sol::this_state L, const sol::object& name, const sol::object& value) {
            queue_client_event(L, "kast.broadcast", ClientEvent::To::all, 0, name, value);
        });

        const auto logger = [this](void (*write)(std::string_view, nlohmann::json)) {
            return [this, write](sol::this_state L, const sol::object& message) {
                write(std::format("{}: {}", who(), to_string(L, message)), nlohmann::json::object());
            };
        };
        api.set_function("log", logger(log::info));
        api.set_function("warn", logger(log::warn));
        api.set_function("error", logger(log::error));
    }
};

Runtime::Runtime(entt::registry& reg, Defs defs, NpcHooks npcs)
    : impl_(std::make_unique<Impl>(reg, std::move(defs), std::move(npcs))) {}
Runtime::~Runtime() = default;

std::vector<std::string> Runtime::load(const std::filesystem::path& root, std::vector<std::string> gameMachines) {
    // Definitions first: an init may already use kast.fsm.
    std::vector<std::string> errors = impl_->load_machines(root, std::move(gameMachines));
    for (std::string& e : impl_->load_npcs(root)) errors.push_back(std::move(e)); // before init: it may kast.create one
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root / "systems", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".lua") files.push_back(it->path());
    std::ranges::sort(files, {}, [](const auto& f) { return f.filename(); });

    impl_->systems.reserve(files.size()); // subscriptions point at their System
    for (const auto& file : files) {
        System& system = impl_->systems.emplace_back();
        system.path = "systems/" + file.filename().generic_string();
        system.file = file;
        if (std::string error = impl_->load(system); !error.empty()) {
            impl_->disable(system, error);
            errors.push_back(std::move(error));
        }
    }
    if (errors.empty()) impl_->fsm.enterAll(); // the server won't start otherwise
    return errors;
}

void Runtime::update(float dt) {
    for (System& s : impl_->systems) impl_->run(s, s.update, "update", dt);
}

void Runtime::enter(entt::entity e) {
    for (System& s : impl_->systems) impl_->run(s, s.onEnter, "onEnter", wire(e));
}

void Runtime::leave(entt::entity e) {
    for (System& s : impl_->systems) impl_->run(s, s.onLeave, "onLeave", wire(e));
}

void Runtime::shutdown() {
    for (System& s : impl_->systems) impl_->run(s, s.shutdown, "shutdown");
    impl_->bus.dropQueued();
}

void Runtime::drain_events() { impl_->bus.drain(); }

EventBus& Runtime::bus() { return impl_->bus; }

StateMachines& Runtime::fsm() { return impl_->fsm; }

std::vector<ClientEvent> Runtime::take_client_events() { return std::exchange(impl_->clientEvents, {}); }

const NpcDef* Runtime::npc(std::string_view name) const {
    const auto it = impl_->npcDefs.find(name);
    return it == impl_->npcDefs.end() ? nullptr : &it->second;
}

std::vector<SystemStatus> Runtime::status() const {
    std::vector<SystemStatus> out;
    for (const System& s : impl_->systems) out.push_back({s.path, s.ok, s.error});
    return out;
}

} // namespace kast::lua
