#pragma once

#include "components.h"

#include <sol/sol.hpp>

#include <filesystem>
#include <string>

// Sandbox and value conversion shared by Lua systems (lua_systems.h, one state per Play session) and Lua component
// definitions (lua_components.h, a state per definition scan). Engine-internal: needs sol2 (a private dependency).
namespace kast {

// Opens only the base, table, string and math libraries and removes the base functions that load code or reach
// outside an environment: no io, os, ffi, debug, jit, bit, package, require, dofile, loadfile, load, loadstring.
void openSandbox(sol::state& lua);
// A fresh environment with its own copies of the globals and library tables, so one file's globals and edits never
// reach another.
sol::environment sandboxEnvironment(sol::state& lua);
// Reads file and loads it as text (precompiled bytecode can break out of the sandbox) into env. chunkName names it in
// error messages ("@path" gives "path:line: ..."). On failure returns an invalid function and sets error.
sol::protected_function loadSandboxed(sol::state& lua, const std::filesystem::path& file, const std::string& chunkName,
                                      const sol::environment& env, std::string& error);

std::string toString(lua_State* L, const sol::object& value); // Lua tostring

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
