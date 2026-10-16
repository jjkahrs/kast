#include "runtime.h"
#include "../core/log.h"
#include "../world/components.h"
#include "sandbox.h"

#include <messages.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace kast::lua {
namespace {

// Raises a Lua error at the calling script's file:line. sol2 turns the exception into lua_error.
[[noreturn]] void raise(lua_State* L, const std::string& message) {
    luaL_where(L, 1);
    std::string where = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error(where + message);
}

// The read-only built-ins every character has, plus the Lua component types.
enum class Builtin { none, character, transform };
struct Type {
    Builtin builtin = Builtin::none;
    const ComponentDef* def = nullptr;
    std::string_view name() const { return def ? std::string_view(def->name) : builtin == Builtin::character ? "Character" : "Transform"; }
};

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

} // namespace

struct Runtime::Impl {
    entt::registry& reg;
    Defs defs;
    sol::state lua;
    sol::table api;
    EventBus bus;                          // after lua: Lua subscribers hold functions of that state
    std::vector<System> systems;           // after bus: their subscriptions unsubscribe from it
    std::vector<ClientEvent> clientEvents; // kast.sendTo/sendNearby/broadcast since the last take
    System* current = nullptr;

    Impl(entt::registry& r, Defs d) : reg(r), defs(std::move(d)) {
        open_sandbox(lua);
        bind_api();
    }

    std::string who() const { return current ? current->path : std::string("lua"); }

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
        env["kast"] = read_only(api);
        return env;
    }

    // Entities are server wire ids; only player characters exist.
    entt::entity entity(lua_State* L, const sol::object& id) {
        if (id.get_type() == sol::type::number) {
            const double value = id.as<double>();
            if (value >= 0.0 && value <= 4294967295.0 && value == std::floor(value)) {
                const auto e = entt::entity{static_cast<uint32_t>(value)};
                if (reg.valid(e) && reg.all_of<world::CharacterRef>(e)) return e;
            }
        }
        raise(L, std::format("invalid entity id {}", to_string(L, id)));
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
        for (const auto& def : defs)
            if (def->name == name) return {Builtin::none, def.get()};
        raise(L, std::format("unknown component \"{}\"", name));
    }

    bool has(entt::entity e, const Type& t) { return t.builtin != Builtin::none || pool(reg, t.def->name).contains(e); }

    const ComponentDef& writable(lua_State* L, const Type& t) {
        if (!t.def) raise(L, std::format("\"{}\" is read-only on the server", t.name()));
        return *t.def;
    }

    void changed(entt::entity e, const ComponentDef& def) {
        if (def.persist) reg.emplace_or_replace<world::Dirty>(e); // replication diffs on its own
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
            for (auto e : reg.view<world::CharacterRef>())
                if (std::ranges::all_of(types, [&](const Type& t) { return has(e, t); })) matches.push_back(wire(e));
            std::ranges::sort(matches);
            // nil ends the generic for.
            return std::function<sol::object()>([this, matches = std::move(matches), next = size_t{0}]() mutable {
                while (next < matches.size()) {
                    const uint32_t id = matches[next++];
                    if (const auto e = entt::entity{id}; reg.valid(e) && reg.all_of<world::CharacterRef>(e))
                        return sol::make_object(lua, id);
                }
                return sol::make_object(lua, sol::lua_nil);
            });
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
            } else if (t.builtin == Builtin::transform) {
                const auto& tf = reg.get<world::Transform>(e);
                table["position"] = lua.create_table_with("x", tf.pos.x, "y", tf.pos.y, "z", tf.pos.z);
                table["heading"] = tf.heading;
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
            const ComponentDef& def = writable(L, t);
            auto& p = pool(reg, def.name);
            Json json = p.get(e).value;
            std::string error;
            for (const auto& [key, value] : fields) {
                if (key.get_type() != sol::type::string)
                    raise(L, std::format("\"{}\": field names must be strings, got {}", name, to_string(L, key)));
                const std::string field = key.as<std::string>();
                const auto desc = std::ranges::find(def.fields, field, &FieldDesc::name);
                if (desc == def.fields.end()) raise(L, std::format("\"{}\": unknown field \"{}\"", name, field));
                if (!lua_to_field_json(*desc, value, json[field], error)) raise(L, error);
            }
            std::optional<Json> valid = component_from_json(def, json, error);
            if (!valid) raise(L, std::format("\"{}\": {}", name, error));
            if (*valid == p.get(e).value) return;
            p.get(e).value = std::move(*valid);
            changed(e, def);
        });

        // Events: queued, delivered by drain_events() after every update (README "Events"). Subscriptions belong to
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

        // Client events: queued, sent as ServerEvent after component replication (README "Events").
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

Runtime::Runtime(entt::registry& reg, Defs defs) : impl_(std::make_unique<Impl>(reg, std::move(defs))) {}
Runtime::~Runtime() = default;

std::vector<std::string> Runtime::load(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root / "systems", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".lua") files.push_back(it->path());
    std::ranges::sort(files, {}, [](const auto& f) { return f.filename(); });

    std::vector<std::string> errors;
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

std::vector<ClientEvent> Runtime::take_client_events() { return std::exchange(impl_->clientEvents, {}); }

std::vector<SystemStatus> Runtime::status() const {
    std::vector<SystemStatus> out;
    for (const System& s : impl_->systems) out.push_back({s.path, s.ok, s.error});
    return out;
}

} // namespace kast::lua
