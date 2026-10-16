#include "state_machines.h"
#include "../core/log.h"
#include "components.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace kast::lua {

bool StateMachineDef::allows(const std::string& from, const std::string& to) const {
    const auto it = states.find(to);
    return it != states.end() && (it->second.fromAny || it->second.from.contains(from));
}

std::vector<std::string> StateMachineDef::stateNames() const {
    std::vector<std::string> names;
    for (const auto& [state, rules] : states) names.push_back(state);
    return names;
}

std::vector<std::string> StateMachines::setup(std::vector<StateMachineDef> defs, std::vector<std::string> gameMachines) {
    for (StateMachineDef& def : defs) {
        const std::string name = def.name;
        defs_.insert_or_assign(name, std::move(def));
    }
    gameList_ = std::move(gameMachines);
    std::vector<std::string> errors;
    for (const std::string& name : gameList_)
        if (!defs_.contains(name))
            errors.push_back(std::format("GAME_STATE_MACHINES: game state machine \"{}\" has no valid definition in {}/",
                                         name, kStateMachinesDir));
    return errors;
}

const StateMachineDef* StateMachines::definition(const std::string& name) const {
    const auto it = defs_.find(name);
    return it == defs_.end() ? nullptr : &it->second;
}

void StateMachines::enterAll() {
    for (const std::string& name : gameList_)
        if (!game_.contains(name))
            if (const auto it = defs_.find(name); it != defs_.end()) enterInitial(name, it->second, it->second.initial);
}

entt::entity StateMachines::find(EntityId id) const {
    const auto entity = entt::entity{id};
    return reg_.valid(entity) ? entity : entt::null;
}

std::optional<std::string> StateMachines::machineOf(entt::entity entity) {
    auto& p = pool(reg_, kStateMachineComponent);
    if (!p.contains(entity)) return std::nullopt;
    return p.get(entity).value.at("machine").get<std::string>();
}

std::string StateMachines::entityInitial(entt::entity entity, const StateMachineDef& def, bool warn) {
    const std::string wanted = pool(reg_, kStateMachineComponent).get(entity).value.at("initialState").get<std::string>();
    if (def.states.contains(wanted)) return wanted;
    if (warn) {
        const std::string problem =
            wanted.empty() ? std::string("is empty") : std::format("\"{}\" is not one of its states", wanted);
        log::warn(std::format("entity {}: StateMachine \"{}\": initialState {}; using \"{}\"", entt::to_integral(entity),
                              def.name, problem, def.initial));
    }
    return def.initial;
}

void StateMachines::syncEntity(entt::entity entity) {
    if (!reg_.valid(entity)) return;
    const std::optional<std::string> name = machineOf(entity); // a copy: enter may change the component
    if (!name) {
        reg_.remove<StateMachineRuntime>(entity);
        return;
    }
    if (const auto* runtime = reg_.try_get<StateMachineRuntime>(entity); runtime && runtime->machine == *name) return;
    reg_.emplace_or_replace<StateMachineRuntime>(entity, *name, std::string());
    const auto it = defs_.find(*name);
    if (it == defs_.end()) {
        if (!name->empty()) // empty: no machine chosen yet (kast.add before kast.set)
            log::error(std::format("entity {}: StateMachine: no valid state machine definition \"{}\"",
                                   entt::to_integral(entity), *name));
        return;
    }
    enterInitial(entt::to_integral(entity), it->second, entityInitial(entity, it->second, true));
}

StateMachines::Instance StateMachines::resolve(const Target& target, bool enter) {
    if (const auto* name = std::get_if<std::string>(&target)) {
        if (std::ranges::find(gameList_, *name) == gameList_.end())
            throw std::invalid_argument(std::format("no game state machine \"{}\" (GAME_STATE_MACHINES)", *name));
        const auto it = defs_.find(*name);
        if (it == defs_.end())
            throw std::invalid_argument(std::format("game state machine \"{}\" has no valid definition", *name));
        if (enter && !game_.contains(*name)) enterInitial(*name, it->second, it->second.initial);
        const auto current = game_.find(*name);
        return {&it->second, current == game_.end() ? std::string() : current->second, it->second.initial, std::nullopt};
    }
    const EntityId id = std::get<EntityId>(target);
    const entt::entity entity = find(id);
    if (entity == entt::null) throw std::invalid_argument(std::format("invalid entity id {}", id));
    if (!machineOf(entity)) throw std::invalid_argument(std::format("entity {} has no \"StateMachine\"", id));
    if (enter) syncEntity(entity);
    const std::optional<std::string> name = reg_.valid(entity) ? machineOf(entity) : std::nullopt;
    if (!name) // removed by its own enter
        throw std::invalid_argument(std::format("entity {} has no \"StateMachine\"", id));
    if (name->empty()) throw std::invalid_argument(std::format("entity {}: StateMachine has no \"machine\"", id));
    const auto it = defs_.find(*name);
    if (it == defs_.end())
        throw std::invalid_argument(std::format("entity {}: no valid state machine definition \"{}\"", id, *name));
    const auto* runtime = reg_.try_get<StateMachineRuntime>(entity);
    const bool entered = runtime && runtime->machine == *name && !runtime->current.empty();
    return {&it->second, entered ? runtime->current : std::string(), entityInitial(entity, it->second, false), id};
}

std::string* StateMachines::slot(const Target& target, const std::string& machine) {
    if (const auto* name = std::get_if<std::string>(&target)) {
        const auto it = game_.find(*name);
        return it == game_.end() ? nullptr : &it->second;
    }
    const entt::entity entity = find(std::get<EntityId>(target));
    if (entity == entt::null) return nullptr;
    auto* runtime = reg_.try_get<StateMachineRuntime>(entity);
    return runtime && runtime->machine == machine && machineOf(entity) == machine ? &runtime->current : nullptr;
}

void StateMachines::enterInitial(const Target& target, const StateMachineDef& def, const std::string& initial) {
    if (const auto* name = std::get_if<std::string>(&target)) {
        game_[*name] = initial;
    } else if (std::string* current = slot(target, def.name)) {
        *current = initial;
    } else {
        return;
    }
    const bool outer = !busy_;
    busy_ = true;
    const std::optional<EntityId> entity =
        std::holds_alternative<EntityId>(target) ? std::optional(std::get<EntityId>(target)) : std::nullopt;
    call(def.states.at(initial).enter, def, "enter", initial, {def.name, entity, std::nullopt, initial});
    if (outer) {
        busy_ = false;
        runQueue();
    }
}

std::string StateMachines::label(const Instance& machine) const {
    return machine.entity ? std::format("State machine \"{}\" (entity {})", machine.def->name, *machine.entity)
                          : std::format("State machine \"{}\"", machine.def->name);
}

bool StateMachines::apply(const Target& target, const std::string& to) {
    const Instance machine = resolve(target, true);
    if (machine.current.empty()) // its first enter removed it again
        throw std::invalid_argument(std::format("{} has no current state", label(machine)));
    if (!machine.def->states.contains(to)) {
        log::warn(std::format("{}: unknown state \"{}\"; staying in \"{}\"", label(machine), to, machine.current));
        return false;
    }
    if (!machine.def->allows(machine.current, to)) {
        log::warn(std::format("{}: \"{}\" can't be entered from \"{}\" (allowedFrom); staying in \"{}\"", label(machine),
                              to, machine.current, machine.current));
        return false;
    }
    const StateMachineDef& def = *machine.def; // definitions never change after setup
    const Transition transition{def.name, machine.entity, machine.current, to};
    busy_ = true;
    call(def.states.at(machine.current).exit, def, "exit", machine.current, transition);
    if (std::string* current = slot(target, def.name)) { // gone if exit despawned, removed or swapped the machine
        *current = to;
        call(def.states.at(to).enter, def, "enter", to, transition);
        bus_.publish(kStateChangedEvent,
                     {{"machine", def.name},
                      {"entity", machine.entity ? nlohmann::json(*machine.entity) : nlohmann::json(nullptr)},
                      {"from", machine.current},
                      {"to", to}},
                     def.path);
    }
    busy_ = false;
    return true;
}

void StateMachines::runQueue() {
    while (!queue_.empty() && !busy_) {
        Queued next = std::move(queue_.front());
        queue_.pop_front();
        try {
            apply(next.target, next.to);
        } catch (const std::exception& e) { // nothing may escape to the tick
            log::warn(std::format("Queued state change to \"{}\" skipped: {}", next.to, e.what()));
        }
    }
}

void StateMachines::call(const StateCallback& callback, const StateMachineDef& def, const char* what,
                         const std::string& state, const Transition& t) {
    if (!callback) return;
    try {
        callback(t);
    } catch (const std::exception& e) { // C++ callbacks; Lua ones log their own errors
        log::error(std::format("State machine {}: {} \"{}\" failed: {}", def.path, what, state, e.what()));
    }
}

std::string StateMachines::state(const Target& target) { return resolve(target, true).current; }

bool StateMachines::change(const Target& target, const std::string& to) {
    if (busy_) {
        resolve(target, true); // a bad target throws at the call
        queue_.push_back({target, to});
        return true;
    }
    const bool changed = apply(target, to);
    runQueue();
    return changed;
}

bool StateMachines::canChange(const Target& target, const std::string& to) {
    const Instance machine = resolve(target, false);
    return machine.def->allows(machine.current.empty() ? machine.initial : machine.current, to);
}

std::vector<std::string> StateMachines::states(const Target& target) { return resolve(target, false).def->stateNames(); }

} // namespace kast::lua
