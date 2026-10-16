// Finite state machines: named states with optional allowedFrom rules and enter/exit callbacks, defined by Lua files
// under LUA_ROOT/fsms/ (scanned recursively, one machine per file). Copied from engine/state_machines.h (same rules,
// warnings and fsm.changed event) without the editor's display helpers (peek, gameStates), Play sessions and scene
// switches (reset, attach, detach), hot reload (define replacing running definitions, rescans) and the per-frame
// syncAll. Game machines (GAME_STATE_MACHINES) run for the server's lifetime; entity machines come from the
// StateMachine built-in on characters. One per lua::Runtime (fsm()). See README "Lua components and systems".
#pragma once
#include "event_bus.h"

#include <entt/entity/registry.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace kast::lua {

constexpr const char* kStateMachinesDir = "fsms";
constexpr const char* kAnyState = "*";
constexpr const char* kStateChangedEvent = "fsm.changed";
constexpr const char* kStateMachineComponent = "StateMachine"; // the built-in: { machine, initialState }

using EntityId = uint32_t; // a character's wire id

// What enter/exit callbacks get. from is empty when a machine enters its first state.
struct Transition {
    std::string machine;
    std::optional<EntityId> entity; // empty for game machines
    std::optional<std::string> from;
    std::string to;
};
using StateCallback = std::function<void(const Transition&)>;

struct StateDef {
    bool fromAny = true;        // allowedFrom omitted or holding "*"
    std::set<std::string> from; // allowedFrom otherwise
    StateCallback enter, exit;  // optional
};

struct StateMachineDef {
    std::string name;
    std::string path; // LUA_ROOT-relative definition file
    std::string initial;
    std::map<std::string, StateDef> states; // by name

    // to is a state that may be entered from from (also when from == to).
    bool allows(const std::string& from, const std::string& to) const;
    std::vector<std::string> stateNames() const; // sorted
};

// The current states of the game machines and of every character's machine. No Lua dependency: callbacks are plain
// std::functions (Runtime binds the Lua ones). Entity machines read the StateMachine component's pool (components.h).
//
// Errors: a bad target (unknown or unavailable game machine, invalid entity, entity without StateMachine or with no
// valid machine) throws std::invalid_argument; kast.fsm turns it into a Lua error at the calling line.
class StateMachines {
public:
    using Target = std::variant<std::string, EntityId>; // a game machine name or an entity id

    StateMachines(EventBus& bus, entt::registry& reg) : bus_(bus), reg_(reg) {}
    StateMachines(const StateMachines&) = delete;
    StateMachines& operator=(const StateMachines&) = delete;

    // The machine's current state. Enters its initial state first if it hasn't yet (a game machine touched during the
    // systems' init, before enterAll).
    std::string state(const Target& target);
    // Applies a transition now: exit(old), switch, enter(new), publish fsm.changed. False (with a warning, nothing
    // run) if the state is unknown or not allowed from the current one. Called during another transition's
    // enter/exit, it is queued, applied after that transition in request order, and returns true.
    bool change(const Target& target, const std::string& to);
    // The same check as change, without side effects or logging.
    bool canChange(const Target& target, const std::string& to);
    std::vector<std::string> states(const Target& target); // sorted

    // --- startup and sync (Runtime) ---

    // Once, before the systems' init: the valid definitions and the game machines to run. Returns one error per listed
    // name without a valid definition.
    std::vector<std::string> setup(std::vector<StateMachineDef> defs, std::vector<std::string> gameMachines);
    const StateMachineDef* definition(const std::string& name) const;
    // After the systems' init: listed game machines enter their initial state. No fsm.changed events.
    void enterAll();
    // A character that gained StateMachine, or whose "machine" changed, enters its initial state; one that lost it
    // forgets its machine. Called after kast.add/kast.set/kast.remove.
    void syncEntity(entt::entity entity);

private:
    struct Queued {
        Target target;
        std::string to;
    };
    // A snapshot of one machine. Re-resolved after callbacks run, as they may change the registry.
    struct Instance {
        const StateMachineDef* def;
        std::string current; // empty: not entered yet
        std::string initial; // what it enters first (entity: a valid initialState, else the definition's initial)
        std::optional<EntityId> entity;
    };

    // Throws std::invalid_argument for a bad target. enter: a machine not entered yet enters its initial state.
    Instance resolve(const Target& target, bool enter);
    // Where machine's current state is stored; null if it is gone (or the entity's StateMachine now names another).
    std::string* slot(const Target& target, const std::string& machine);
    void enterInitial(const Target& target, const StateMachineDef& def, const std::string& initial);
    bool apply(const Target& target, const std::string& to);
    void runQueue();
    void call(const StateCallback& callback, const StateMachineDef& def, const char* what, const std::string& state,
              const Transition& t);
    std::string label(const Instance& machine) const;
    std::string entityInitial(entt::entity entity, const StateMachineDef& def, bool warn);
    entt::entity find(EntityId id) const; // entt::null if no such character
    std::optional<std::string> machineOf(entt::entity entity); // StateMachine "machine"; nullopt without one

    EventBus& bus_;
    entt::registry& reg_;
    std::map<std::string, StateMachineDef> defs_;
    std::vector<std::string> gameList_;
    std::map<std::string, std::string> game_; // name -> current state, for game machines that have entered one
    std::deque<Queued> queue_;
    bool busy_ = false; // a transition's callbacks are running
};

// Runtime-only (never saved or replicated): a character machine's definition name and current state.
struct StateMachineRuntime {
    std::string machine;
    std::string current; // empty if the definition was missing when it tried to enter
};

} // namespace kast::lua
