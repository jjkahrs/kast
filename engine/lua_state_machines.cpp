#include "lua_state_machines.h"

#include "files.h"
#include "log.h"
#include "lua_sandbox.h"
#include "project.h"

#include <algorithm>
#include <format>

namespace kast {
namespace {

using Invalid = InvalidDefinition;

struct ScanState {
    std::optional<std::filesystem::path> root;
    DefinitionFiles files;
    StateMachineDefinitions defs;
};

ScanState& scanned()
{
    static ScanState state;
    return state;
}

std::string text(const sol::table& table, const char* key, const std::string& what)
{
    const sol::object value = table[key];
    if (value.get_type() != sol::type::string || value.as<std::string>().empty())
        throw Invalid(std::format("{}\"{}\" must be a non-empty string", what, key));
    return value.as<std::string>();
}

StateCallback callback(const sol::table& state, const char* key, const std::string& stateName, const std::string& path,
                       const std::string& what, const CallbackBinder& bind)
{
    const sol::object fn = state[key];
    if (fn.get_type() == sol::type::lua_nil)
        return nullptr;
    if (fn.get_type() != sol::type::function)
        throw Invalid(std::format("{}\"{}\" must be a function", what, key));
    return bind ? bind(fn.as<sol::protected_function>(), path, std::format("{} \"{}\"", key, stateName)) : nullptr;
}

StateMachineDef parseDefinition(sol::state& lua, const std::filesystem::path& file, const std::string& path,
                                const sol::environment& env, const CallbackBinder& bind, std::string& declared)
{
    std::string error;
    sol::protected_function body = loadSandboxed(lua, file, "@" + path, env, error);
    if (!body.valid())
        throw Invalid(error.starts_with("cannot open") ? std::format("{}: {}", path, error) : error);
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state()); // per file, in Play's scan and the editor's
        return body();
    }();
    if (!result.valid())
        throw Invalid(result.get<sol::error>().what()); // "path:line: message"
    if (result.return_count() != 1 || result.get_type() != sol::type::table)
        throw Invalid(std::format("{}: must return exactly one table {{name = ..., initial = ..., states = {{...}}}}", path));
    const sol::table table = result;
    const std::string prefix = path + ": ";
    checkPlainData(table, prefix);
    StateMachineDef def;
    def.path = path;
    def.name = text(table, "name", prefix);
    declared = def.name;
    checkKeys(table, {"name", "initial", "states"}, prefix);

    const sol::object states = table["states"];
    if (states.get_type() != sol::type::table)
        throw Invalid(std::format("{}\"states\" must be a table {{name = {{...}}, ...}}", prefix));
    std::vector<std::pair<std::string, sol::table>> entries; // sorted by name, so the first error doesn't vary
    for (const auto& [key, value] : states.as<sol::table>()) {
        if (key.get_type() != sol::type::string || key.as<std::string>().empty() || key.as<std::string>() == kAnyState)
            throw Invalid(std::format("{}state names must be non-empty strings other than \"*\", got {}", prefix,
                                      toString(lua.lua_state(), key)));
        if (value.get_type() != sol::type::table)
            throw Invalid(std::format("{}state \"{}\" must be a table", prefix, key.as<std::string>()));
        entries.emplace_back(key.as<std::string>(), value.as<sol::table>());
    }
    if (entries.empty())
        throw Invalid(std::format("{}\"states\" must not be empty", prefix));
    std::ranges::sort(entries, {}, &std::pair<std::string, sol::table>::first);
    for (const auto& [name, state] : entries)
        def.states[name];
    for (const auto& [name, state] : entries) {
        const std::string what = std::format("{}state \"{}\": ", prefix, name);
        checkKeys(state, {"allowedFrom", "enter", "exit"}, what);
        StateDef& rules = def.states[name];
        if (const sol::object from = state["allowedFrom"]; from.get_type() != sol::type::lua_nil) {
            rules.fromAny = false;
            for (const sol::object& item : sequence(from, what + "\"allowedFrom\"")) {
                if (item.get_type() != sol::type::string)
                    throw Invalid(std::format("{}\"allowedFrom\" must hold state names, got {}", what,
                                              toString(lua.lua_state(), item)));
                const std::string source = item.as<std::string>();
                if (source == kAnyState)
                    rules.fromAny = true;
                else if (!def.states.contains(source))
                    throw Invalid(std::format("{}\"allowedFrom\" names unknown state \"{}\"", what, source));
                else
                    rules.from.insert(source);
            }
        }
        rules.enter = callback(state, "enter", name, path, what, bind);
        rules.exit = callback(state, "exit", name, path, what, bind);
    }

    const sol::object initial = table["initial"];
    if (initial.get_type() != sol::type::string || !def.states.contains(initial.as<std::string>()))
        throw Invalid(std::format("{}\"initial\" must name one of its states, got {}", prefix,
                                  toString(lua.lua_state(), initial)));
    def.initial = initial.as<std::string>();
    return def;
}

} // namespace

DefinitionFiles scanStateMachineFiles(const std::filesystem::path& root)
{
    DefinitionFiles files;
    std::error_code ec;
    for (const std::filesystem::path& file : listFiles(root / kStateMachinesDir))
        if (file.extension() == ".lua")
            files[file] = std::filesystem::last_write_time(file, ec); // disk only: under a pak, never changes
    return files;
}

StateMachineDefinitions parseStateMachineDefinitions(sol::state& lua, const std::filesystem::path& root,
                                                     const std::function<sol::environment()>& environment,
                                                     const CallbackBinder& bind)
{
    StateMachineDefinitions out;
    std::vector<std::optional<StateMachineDef>> parsed; // parallel to out.status
    for (const auto& [file, mtime] : scanStateMachineFiles(root)) {
        ComponentDefinitionStatus status;
        status.path = toProjectPath(root, file).value_or(file.generic_string());
        std::optional<StateMachineDef> def;
        try {
            def = parseDefinition(lua, file, status.path, environment(), bind, status.name);
            status.ok = true;
        } catch (const Invalid& e) {
            status.error = e.what();
        } catch (const std::exception& e) { // sol2 conversions
            status.error = std::format("{}: {}", status.path, e.what());
        }
        out.status.push_back(std::move(status));
        parsed.push_back(std::move(def));
    }
    // Name clashes reject every file of a name, so the result doesn't depend on scan order.
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (!parsed[i])
            continue;
        std::string others;
        for (size_t j = 0; j < parsed.size(); ++j)
            if (j != i && out.status[j].name == parsed[i]->name)
                others += std::format("{}{}", others.empty() ? "" : ", ", out.status[j].path);
        if (!others.empty()) {
            out.status[i].ok = false;
            out.status[i].error =
                std::format("{}: name \"{}\" is also declared in {}", out.status[i].path, parsed[i]->name, others);
        }
    }
    for (size_t i = 0; i < parsed.size(); ++i)
        if (out.status[i].ok)
            out.valid.push_back(std::move(*parsed[i]));
    return out;
}

StateMachineDefinitions loadStateMachineDefinitions(const std::filesystem::path& projectRoot)
{
    sol::state lua;
    openSandbox(lua);
    return parseStateMachineDefinitions(lua, projectRoot, [&] { return sandboxEnvironment(lua); }, nullptr);
}

bool refreshStateMachineDefinitions(const std::optional<std::filesystem::path>& root)
{
    ScanState& state = scanned();
    DefinitionFiles files = root ? scanStateMachineFiles(*root) : DefinitionFiles{};
    if (root == state.root && files == state.files)
        return false;
    state.defs = root ? loadStateMachineDefinitions(*root) : StateMachineDefinitions{};
    for (const ComponentDefinitionStatus& status : state.defs.status)
        if (!status.ok)
            log::error(std::format("State machine rejected: {}", status.error));
    state.root = root;
    state.files = std::move(files);
    return true;
}

const StateMachineDefinitions& stateMachineDefinitions() { return scanned().defs; }

} // namespace kast
