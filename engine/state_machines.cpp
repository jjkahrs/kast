#include "state_machines.h"

#include "log.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace kast {

bool StateMachineDef::allows(const std::string& from, const std::string& to) const
{
    const auto it = states.find(to);
    return it != states.end() && (it->second.fromAny || it->second.from.contains(from));
}

std::vector<std::string> StateMachineDef::stateNames() const
{
    std::vector<std::string> names;
    for (const auto& [state, rules] : states)
        names.push_back(state);
    return names;
}

void StateMachines::reset(std::vector<std::string> gameMachines)
{
    defs_.clear();
    game_.clear();
    reported_.clear();
    queue_.clear();
    gameList_ = std::move(gameMachines);
    scene_ = nullptr;
    resolve_ = {};
    busy_ = false;
}

void StateMachines::attach(Scene& scene, Resolver resolve)
{
    scene_ = &scene;
    resolve_ = std::move(resolve);
}

void StateMachines::detach()
{
    for (auto& [name, def] : defs_)
        for (auto& [state, rules] : def->states)
            rules.enter = rules.exit = nullptr;
    queue_.clear();
    scene_ = nullptr;
    resolve_ = {};
    busy_ = false;
}

const StateMachineDef* StateMachines::definition(const std::string& name) const
{
    const auto it = defs_.find(name);
    return it == defs_.end() ? nullptr : it->second.get();
}

void StateMachines::define(StateMachineDef def)
{
    auto shared = std::make_shared<StateMachineDef>(std::move(def));
    const std::string name = shared->name;
    defs_[name] = shared;
    if (!scene_)
        return;
    if (const auto it = game_.find(name); it != game_.end() && !shared->states.contains(it->second)) {
        log::warn(std::format("State machine \"{}\": state \"{}\" no longer exists; reset to \"{}\"", name, it->second,
                              shared->initial));
        enterInitial(name, shared, shared->initial);
    }
    entt::registry& registry = scene_->registry();
    std::vector<entt::entity> entities;
    for (auto [entity, runtime] : registry.view<StateMachineRuntime>().each())
        if (runtime.machine == name)
            entities.push_back(entity);
    for (entt::entity entity : entities) {
        const auto* runtime = registry.try_get<StateMachineRuntime>(entity);
        if (!runtime || runtime->machine != name)
            continue; // changed by an earlier entity's enter
        if (runtime->current.empty()) { // the definition was missing when it tried to enter
            registry.remove<StateMachineRuntime>(entity);
            syncEntity(entity);
        } else if (!shared->states.contains(runtime->current)) {
            const std::string initial = entityInitial(entity, *shared, false);
            log::warn(std::format("{}: state machine \"{}\": state \"{}\" no longer exists; reset to \"{}\"",
                                  entityLabel(registry, entity), name, runtime->current, initial));
            enterInitial(registry.get<PersistentId>(entity).value, shared, initial);
        }
    }
}

void StateMachines::enterAll()
{
    for (const std::string& name : gameList_) {
        if (game_.contains(name))
            continue;
        if (const auto it = defs_.find(name); it != defs_.end())
            enterInitial(name, it->second, it->second->initial);
        else if (reported_.insert(name).second)
            log::error(std::format("Game state machine \"{}\" (project.kast \"stateMachines\") has no valid definition "
                                   "in {}/; it is unavailable",
                                   name, kStateMachinesDir));
    }
    syncAll();
}

std::string StateMachines::entityInitial(entt::entity entity, const StateMachineDef& def, bool warn) const
{
    const entt::registry& registry = scene_->registry();
    const std::string& wanted = registry.get<StateMachine>(entity).initialState;
    if (def.states.contains(wanted))
        return wanted;
    if (warn) {
        const std::string problem =
            wanted.empty() ? std::string("is empty") : std::format("\"{}\" is not one of its states", wanted);
        log::warn(std::format("{}: StateMachine \"{}\": initialState {}; using \"{}\"", entityLabel(registry, entity),
                              def.name, problem, def.initial));
    }
    return def.initial;
}

void StateMachines::syncEntity(entt::entity entity)
{
    if (!scene_ || !scene_->valid(entity))
        return;
    entt::registry& registry = scene_->registry();
    const StateMachine* machine = registry.try_get<StateMachine>(entity);
    if (!machine) {
        registry.remove<StateMachineRuntime>(entity);
        return;
    }
    if (const auto* runtime = registry.try_get<StateMachineRuntime>(entity); runtime && runtime->machine == machine->machine)
        return;
    const std::string name = machine->machine; // a copy: enter may change the component
    registry.emplace_or_replace<StateMachineRuntime>(entity, name, std::string());
    const auto it = defs_.find(name);
    if (it == defs_.end()) {
        if (!name.empty()) // empty: no machine chosen yet (e.g. kast.add before kast.set)
            log::error(std::format("{}: StateMachine: no valid state machine definition \"{}\"",
                                   entityLabel(registry, entity), name));
        return;
    }
    enterInitial(registry.get<PersistentId>(entity).value, it->second, entityInitial(entity, *it->second, true));
}

void StateMachines::syncAll()
{
    if (!scene_)
        return;
    entt::registry& registry = scene_->registry();
    const auto lost = registry.view<StateMachineRuntime>(entt::exclude<StateMachine>);
    registry.remove<StateMachineRuntime>(lost.begin(), lost.end());
    std::vector<entt::entity> entities; // collected first: enter callbacks may change the registry
    scene_->forEachDepthFirst([&](entt::entity entity) {
        if (registry.all_of<StateMachine>(entity))
            entities.push_back(entity);
    });
    for (entt::entity entity : entities)
        syncEntity(entity);
}

StateMachines::Instance StateMachines::resolve(const Target& target, bool enter)
{
    if (!scene_)
        throw std::invalid_argument("no Play session is running");
    if (const auto* name = std::get_if<std::string>(&target)) {
        if (std::ranges::find(gameList_, *name) == gameList_.end())
            throw std::invalid_argument(std::format("no game state machine \"{}\" (project.kast \"stateMachines\")", *name));
        const auto it = defs_.find(*name);
        if (it == defs_.end())
            throw std::invalid_argument(std::format("game state machine \"{}\" has no valid definition", *name));
        if (enter && !game_.contains(*name))
            enterInitial(*name, it->second, it->second->initial);
        const auto current = game_.find(*name);
        return {it->second, current == game_.end() ? std::string() : current->second, it->second->initial, std::nullopt};
    }
    const EntityId id = std::get<EntityId>(target);
    const entt::entity entity = resolve_ ? resolve_(id) : entt::null;
    if (!scene_->valid(entity))
        throw std::invalid_argument(std::format("invalid entity id {}", id));
    entt::registry& registry = scene_->registry();
    if (!registry.all_of<StateMachine>(entity))
        throw std::invalid_argument(std::format("entity {} has no \"StateMachine\"", id));
    if (enter)
        syncEntity(entity);
    if (!scene_->valid(entity) || !registry.all_of<StateMachine>(entity)) // removed by its own enter
        throw std::invalid_argument(std::format("entity {} has no \"StateMachine\"", id));
    const std::string& name = registry.get<StateMachine>(entity).machine;
    if (name.empty())
        throw std::invalid_argument(std::format("entity {}: StateMachine has no \"machine\"", id));
    const auto it = defs_.find(name);
    if (it == defs_.end())
        throw std::invalid_argument(std::format("entity {}: no valid state machine definition \"{}\"", id, name));
    const auto* runtime = registry.try_get<StateMachineRuntime>(entity);
    const bool entered = runtime && runtime->machine == name && !runtime->current.empty();
    return {it->second, entered ? runtime->current : std::string(), entityInitial(entity, *it->second, false), id};
}

std::string* StateMachines::slot(const Target& target)
{
    if (const auto* name = std::get_if<std::string>(&target)) {
        const auto it = game_.find(*name);
        return it == game_.end() ? nullptr : &it->second;
    }
    const entt::entity entity = scene_ && resolve_ ? resolve_(std::get<EntityId>(target)) : entt::null;
    if (!scene_ || !scene_->valid(entity))
        return nullptr;
    auto* runtime = scene_->registry().try_get<StateMachineRuntime>(entity);
    return runtime ? &runtime->current : nullptr;
}

void StateMachines::enterInitial(const Target& target, const std::shared_ptr<StateMachineDef>& def,
                                 const std::string& initial)
{
    if (const auto* name = std::get_if<std::string>(&target)) {
        game_[*name] = initial;
    } else if (std::string* current = slot(target)) {
        *current = initial;
    } else {
        return;
    }
    const bool outer = !busy_;
    busy_ = true;
    const std::optional<EntityId> entity =
        std::holds_alternative<EntityId>(target) ? std::optional(std::get<EntityId>(target)) : std::nullopt;
    call(def->states.at(initial).enter, *def, "enter", initial, {def->name, entity, std::nullopt, initial});
    if (outer) {
        busy_ = false;
        runQueue();
    }
}

std::string StateMachines::label(const Instance& machine) const
{
    return machine.entity ? std::format("State machine \"{}\" (entity {})", machine.def->name, *machine.entity)
                          : std::format("State machine \"{}\"", machine.def->name);
}

bool StateMachines::apply(const Target& target, const std::string& to)
{
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
    const std::shared_ptr<StateMachineDef> def = machine.def; // kept alive through the callbacks
    const Transition transition{def->name, machine.entity, machine.current, to};
    busy_ = true;
    call(def->states.at(machine.current).exit, *def, "exit", machine.current, transition);
    if (std::string* current = slot(target)) { // gone if exit destroyed the entity or removed its StateMachine
        *current = to;
        call(def->states.at(to).enter, *def, "enter", to, transition);
        bus_.publish(kStateChangedEvent,
                     {{"machine", def->name},
                      {"entity", machine.entity ? nlohmann::json(*machine.entity) : nlohmann::json(nullptr)},
                      {"from", machine.current},
                      {"to", to}},
                     def->path);
    }
    busy_ = false;
    return true;
}

void StateMachines::runQueue()
{
    while (!queue_.empty() && !busy_) {
        Queued next = std::move(queue_.front());
        queue_.pop_front();
        try {
            apply(next.target, next.to);
        } catch (const std::invalid_argument& e) {
            log::warn(std::format("Queued state change to \"{}\" skipped: {}", next.to, e.what()));
        }
    }
}

void StateMachines::call(const StateCallback& callback, const StateMachineDef& def, const char* what,
                         const std::string& state, const Transition& t)
{
    if (!callback)
        return;
    try {
        callback(t);
    } catch (const std::exception& e) { // C++ callbacks; Lua ones log their own errors
        log::error(std::format("State machine {}: {} \"{}\" failed: {}", def.path, what, state, e.what()));
    }
}

std::string StateMachines::state(const Target& target)
{
    return resolve(target, true).current;
}

bool StateMachines::change(const Target& target, const std::string& to)
{
    if (busy_) {
        resolve(target, true); // a bad target throws at the call
        queue_.push_back({target, to});
        return true;
    }
    const bool changed = apply(target, to);
    runQueue();
    return changed;
}

bool StateMachines::canChange(const Target& target, const std::string& to)
{
    const Instance machine = resolve(target, false);
    return machine.def->allows(machine.current.empty() ? machine.initial : machine.current, to);
}

std::vector<std::string> StateMachines::states(const Target& target)
{
    return resolve(target, false).def->stateNames();
}

std::optional<std::string> StateMachines::peek(const Target& target) const
{
    try {
        const Instance machine = const_cast<StateMachines*>(this)->resolve(target, false); // enter = false: no change
        return machine.current.empty() ? std::nullopt : std::optional(machine.current);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

std::map<std::string, std::string> StateMachines::gameStates() const { return game_; }

} // namespace kast
