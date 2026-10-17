#pragma once

#include "event_bus.h"
#include "lua_components.h" // ComponentDefinitionStatus: the same status shape for state machine definition files
#include "scene.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

// Finite state machines: named states with optional allowedFrom rules and enter/exit callbacks, defined by Lua files
// under <project>/fsms/ (scanned recursively, one machine per file). Game machines are project-wide (project.kast
// "stateMachines") and keep their state across kast.loadScene; entity machines come from the StateMachine component.
// See docs/engine/state-machines.html.
namespace kast {

constexpr const char* kStateMachinesDir = "fsms";
constexpr const char* kAnyState = "*";
constexpr const char* kStateChangedEvent = "fsm.changed";

// What enter/exit callbacks get. from is empty when a machine enters its first state (or resets after a hot reload).
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
    std::string path; // project-relative definition file
    std::string initial;
    std::map<std::string, StateDef> states; // by name

    // to is a state that may be entered from from (also when from == to).
    bool allows(const std::string& from, const std::string& to) const;
    std::vector<std::string> stateNames() const; // sorted
};

// Runtime: the current states of a Play session's game and entity machines, owned by LuaSystems (fsm()) and used by
// kast.fsm.*. No Lua or ImGui dependency: callbacks are plain std::functions (LuaSystems binds the Lua ones).
//
// Errors: a bad target (unknown or unavailable machine, invalid entity, entity without StateMachine, no session)
// throws std::invalid_argument; kast.fsm turns it into a Lua error at the calling line.
class StateMachines {
public:
    using Target = std::variant<std::string, EntityId>; // a game machine name or an entity id
    using Resolver = std::function<entt::entity(EntityId)>; // entt::null if no such entity

    explicit StateMachines(EventBus& bus) : bus_(bus) {}
    StateMachines(const StateMachines&) = delete;
    StateMachines& operator=(const StateMachines&) = delete;

    // The machine's current state. Enters its initial state first if it hasn't yet (a machine touched during the
    // systems' init, before enterAll).
    std::string state(const Target& target);
    // Applies a transition now: exit(old), switch, enter(new), publish fsm.changed. False (with a Console warning,
    // nothing run) if the state is unknown or not allowed from the current one. Called during another transition's
    // enter/exit, it is queued, applied after that transition in request order, and returns true.
    bool change(const Target& target, const std::string& to);
    // The same check as change, without side effects or logging.
    bool canChange(const Target& target, const std::string& to);
    std::vector<std::string> states(const Target& target); // sorted

    // Read-only, for editor display: the current state if the machine has entered one, never entering it.
    std::optional<std::string> peek(const Target& target) const;
    // Game machines that have entered a state: name -> current state.
    std::map<std::string, std::string> gameStates() const;

    // --- session control (LuaSystems) ---

    // A new Play session: forgets everything, then lists the game machines to run (project.kast "stateMachines").
    void reset(std::vector<std::string> gameMachines = {});
    // Binds a scene (after a switch too). resolve maps ids to the scene's entities.
    void attach(Scene& scene, Resolver resolve);
    // The Lua state is going (scene switch): drops every callback, keeps definitions, game states and the game list.
    void detach();
    // Adds or replaces a definition. Running machines of that name keep their state if it still exists; otherwise
    // they reset to the initial state with a warning (enter runs with no from). An entity machine whose definition
    // was missing enters its initial state.
    void define(StateMachineDef def);
    const StateMachineDef* definition(const std::string& name) const;
    // After the systems' init: listed game machines enter their initial state (a listed name without a definition is
    // logged as an error once per session), then every StateMachine entity depth-first. No fsm.changed events.
    void enterAll();
    // An entity that gained StateMachine, or whose "machine" changed, enters its initial state; one that lost it
    // forgets its machine. Called after kast.add/kast.set and every tick.
    void syncEntity(entt::entity entity);
    void syncAll();

private:
    struct Queued {
        Target target;
        std::string to;
    };
    // A snapshot of one machine: a game machine, or an entity with StateMachine. Re-resolved after callbacks run, as
    // they may change the registry.
    struct Instance {
        std::shared_ptr<StateMachineDef> def;
        std::string current; // empty: not entered yet
        std::string initial; // what it enters first (entity: a valid initialState, else the definition's initial)
        std::optional<EntityId> entity;
    };

    // Throws std::invalid_argument for a bad target. enter: a machine not entered yet enters its initial state.
    Instance resolve(const Target& target, bool enter);
    // Where machine's current state is stored; null if it is gone (or the entity's StateMachine now names another).
    std::string* slot(const Target& target, const std::string& machine);
    void enterInitial(const Target& target, const std::shared_ptr<StateMachineDef>& def, const std::string& initial);
    bool apply(const Target& target, const std::string& to);
    void runQueue();
    void call(const StateCallback& callback, const StateMachineDef& def, const char* what, const std::string& state,
              const Transition& t);
    std::string label(const Instance& machine) const;
    std::string entityInitial(entt::entity entity, const StateMachineDef& def, bool warn) const;

    EventBus& bus_;
    Scene* scene_ = nullptr;
    Resolver resolve_;
    std::map<std::string, std::shared_ptr<StateMachineDef>> defs_;
    std::vector<std::string> gameList_;
    std::map<std::string, std::string> game_; // name -> current state, for game machines that have entered one
    std::set<std::string> reported_;          // listed game machines already logged as missing this session
    std::deque<Queued> queue_;
    bool busy_ = false; // a transition's callbacks are running
};

// Runtime-only (never saved or copied): an entity machine's definition name and current state.
struct StateMachineRuntime {
    std::string machine;
    std::string current; // empty if the definition was missing when it tried to enter
};

// --- Definition files, for the editor (Inspector dropdowns, State Machines panel) ---

struct StateMachineDefinitions {
    std::vector<StateMachineDef> valid;            // no callbacks
    std::vector<ComponentDefinitionStatus> status; // one per file, sorted by path
};
// Loads and checks every definition under projectRoot/fsms/ in a fresh sandboxed Lua state. Each file runs in its own
// environment (no kast API: callbacks are not run).
StateMachineDefinitions loadStateMachineDefinitions(const std::filesystem::path& projectRoot);
// Rescans if root changed or a .lua file under its fsms/ was added, removed or modified since the last scan; logs each
// rejected file. nullopt clears them. Returns true if it rescanned.
// ponytail: rescans every definition on any change; fine for tens of files.
bool refreshStateMachineDefinitions(const std::optional<std::filesystem::path>& root);
const StateMachineDefinitions& stateMachineDefinitions(); // of the last scan

} // namespace kast
