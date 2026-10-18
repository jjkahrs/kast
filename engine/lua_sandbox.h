#pragma once

#include "components.h"

#include <sol/sol.hpp>

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

// Sandbox and value conversion shared by Lua systems (lua_systems.h, one state per Play session) and Lua component
// definitions (lua_components.h, a state per definition scan). Engine-internal: needs sol2 (a private dependency).
namespace kast {

// Opens only the base, table, string and math libraries and removes the base functions that load code or reach
// outside an environment: no io, os, ffi, debug, jit, bit, package, require, dofile, loadfile, load, loadstring.
// Also installs the runaway-script watchdog (see ScriptBudget).
void openSandbox(sol::state& lua);
// A fresh environment with its own copies of the globals and library tables, so one file's globals and edits never
// reach another.
sol::environment sandboxEnvironment(sol::state& lua);
// Reads file and loads it as text (precompiled bytecode can break out of the sandbox) into env. chunkName names it in
// error messages ("@path" gives "path:line: ..."). On failure returns an invalid function and sets error.
sol::protected_function loadSandboxed(sol::state& lua, const std::filesystem::path& file, const std::string& chunkName,
                                      const sol::environment& env, std::string& error);

// Runaway-script watchdog: each entry into Lua from C++ (a system's chunk or callback, a state machine callback, a
// definition file) holds a ScriptBudget, and may run kScriptBudget of wall-clock time before a count hook raises a Lua
// error, which callers report like any other error. Count hooks never fire in JIT-compiled code; the sandbox doesn't
// open the jit library, which leaves LuaJIT's JIT compiler off.
constexpr std::chrono::seconds kScriptBudget{2};
// Starts the budget for an outermost entry into Lua; nested entries (kast.fsm.change running a callback) share it.
class ScriptBudget {
public:
    explicit ScriptBudget(lua_State* L);
    ~ScriptBudget();
    ScriptBudget(const ScriptBudget&) = delete;
    ScriptBudget& operator=(const ScriptBudget&) = delete;

private:
    lua_State* L_;
    bool outer_;
};

std::string toString(lua_State* L, const sol::object& value); // Lua tostring
// Raises a Lua error at the calling script's file:line. sol2 turns the exception into lua_error.
[[noreturn]] void raise(lua_State* L, const std::string& message);

// Definition-file checks shared by Lua components and state machines. Each throws InvalidDefinition, its message
// starting with what (e.g. "components/health.lua: ").
struct InvalidDefinition : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Every key of table is a string in allowed.
void checkKeys(const sol::table& table, std::initializer_list<const char*> allowed, const std::string& what);
// No table reachable from table (keys or values) has a metatable, so reading the definition runs no Lua code (an
// endless __index would escape the watchdog: sol2's table reads aren't protected calls). Checked without running any.
void checkPlainData(const sol::table& table, const std::string& what);
// value is a sequence {v1, v2, ...} (exactly the keys 1..n); returns its items.
std::vector<sol::object> sequence(const sol::object& value, const std::string& what);

// A Lua value as the JSON a component field takes (or one array element when element is true): {x=,y=,z=[,w=]} tables
// become arrays, 1-based sequences become arrays, integral numbers integers. Shape errors set error ("\"f\" must be
// ...") and return false; type and range checks are left to the component's JSON checks (other types become null).
bool luaToFieldJson(const FieldDesc& field, const sol::object& value, nlohmann::ordered_json& out, std::string& error,
                    bool element = false);
// The reverse, for a valid canonical field value: arrays become 1-based sequences.
sol::object fieldJsonToLua(sol::state_view lua, const FieldDesc& field, const nlohmann::ordered_json& value,
                           bool element = false);

// An event payload (plain data) as JSON: nil -> null, booleans, finite numbers (integral ones as integers), strings,
// and tables whose keys are exactly 1..n (arrays) or all strings (objects; an empty table is {}), nested at most
// kMaxPayloadDepth tables deep. Anything else (functions, userdata, threads, cycles, mixed keys) sets error and
// returns false.
constexpr int kMaxPayloadDepth = 32;
bool luaToJson(const sol::object& value, nlohmann::json& out, std::string& error);
// The reverse: arrays become 1-based sequences, objects string-keyed tables, null nil. Always a fresh value.
sol::object jsonToLua(sol::state_view lua, const nlohmann::json& value);

} // namespace kast
