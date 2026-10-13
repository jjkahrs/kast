#pragma once

#include "state_machines.h"

#include <sol/sol.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <string>

// State machine definition files (<project>/fsms/**/*.lua) read with Lua: shared by the editor's scan
// (loadStateMachineDefinitions, no callbacks) and Play's (LuaSystems, callbacks bound to its Lua state).
// Engine-internal: needs sol2 (a private dependency).
namespace kast {

using DefinitionFiles = std::map<std::filesystem::path, std::filesystem::file_time_type>;

// Every .lua file under root/fsms/ with its modification time; empty if there is no such folder.
DefinitionFiles scanStateMachineFiles(const std::filesystem::path& root);

// Turns a state's enter or exit function into a callback. what is e.g. "enter \"open\"".
using CallbackBinder =
    std::function<StateCallback(sol::protected_function fn, const std::string& path, const std::string& what)>;

// Loads and checks every file of scanStateMachineFiles(root), each run in its own environment() (a sandboxed one).
// With no bind, valid definitions have no callbacks. Duplicate names reject every file of that name.
StateMachineDefinitions parseStateMachineDefinitions(sol::state& lua, const std::filesystem::path& root,
                                                     const std::function<sol::environment()>& environment,
                                                     const CallbackBinder& bind);

} // namespace kast
