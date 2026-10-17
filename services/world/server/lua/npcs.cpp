#include "npcs.h"
#include "sandbox.h"
#include "state_machines.h"

#include <algorithm>
#include <format>

namespace kast::lua {
namespace {

using Invalid = InvalidDefinition;

constexpr size_t kMaxNameBytes = 255; // name and template, as kast.create allows
constexpr const char* kReadOnly[] = {"Character", "Npc", "Spawn", "Transform"};

std::string text(const sol::table& table, const char* key, const std::string& what) {
    const sol::object value = table[key];
    if (value.get_type() != sol::type::string || value.as<std::string>().empty() ||
        value.as<std::string>().size() > kMaxNameBytes)
        throw Invalid(std::format("{}\"{}\" must be a non-empty string of at most {} bytes", what, key, kMaxNameBytes));
    return value.as<std::string>();
}

NpcDef parse_definition(sol::state& lua, const std::filesystem::path& file, const std::string& path, const Defs& defs,
                        const MachineExists& machine, std::string& declared) {
    std::string error;
    sol::protected_function body = load_sandboxed(lua, file, "@" + path, sandbox_environment(lua), error);
    if (!body.valid()) throw Invalid(error.starts_with("cannot open") ? std::format("{}: {}", path, error) : error);
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state());
        return body();
    }();
    if (!result.valid()) throw Invalid(result.get<sol::error>().what()); // "path:line: message"
    if (result.return_count() != 1 || result.get_type() != sol::type::table)
        throw Invalid(std::format("{}: must return exactly one table {{name = ..., template = ..., components = {{...}}}}", path));
    const sol::table table = result;
    const std::string prefix = path + ": ";
    check_plain_data(table, prefix);
    NpcDef def;
    def.name = text(table, "name", prefix);
    declared = def.name;
    check_keys(table, {"name", "template", "components"}, prefix);
    def.templateName = text(table, "template", prefix);
    std::optional<Json> components = merge_components(table["components"], Json::object(), defs, machine, error);
    if (!components) throw Invalid(prefix + error);
    def.components = std::move(*components);
    return def;
}

} // namespace

std::optional<Json> merge_components(const sol::object& components, Json base, const Defs& defs,
                                     const MachineExists& machine, std::string& error) {
    if (components.get_type() == sol::type::lua_nil) return base;
    lua_State* L = components.lua_state();
    if (components.get_type() != sol::type::table) {
        error = std::format("\"components\" must be a table {{Type = {{field = value, ...}}, ...}}, got {}",
                            to_string(L, components));
        return std::nullopt;
    }
    std::vector<std::pair<std::string, sol::object>> entries; // sorted by name, so the first error doesn't vary
    for (const auto& [key, fields] : components.as<sol::table>()) {
        if (key.get_type() != sol::type::string) {
            error = std::format("\"components\": component names must be strings, got {}", to_string(L, key));
            return std::nullopt;
        }
        entries.emplace_back(key.as<std::string>(), fields);
    }
    std::ranges::sort(entries, {}, &std::pair<std::string, sol::object>::first);
    for (const auto& [name, fields] : entries) {
        if (std::ranges::any_of(kReadOnly, [&](const char* n) { return name == n; })) {
            error = std::format("\"{}\" is read-only on the server", name);
            return std::nullopt;
        }
        const auto def = std::ranges::find_if(defs, [&](const auto& d) { return d->name == name; });
        if (def == defs.end()) {
            error = std::format("unknown component \"{}\"", name);
            return std::nullopt;
        }
        if (fields.get_type() != sol::type::table) {
            error = std::format("\"{}\" must be a table {{field = value, ...}}, got {}", name, to_string(L, fields));
            return std::nullopt;
        }
        std::optional<Json> value = set_fields(**def, base.contains(name) ? base[name] : default_component(**def),
                                               fields.as<sol::table>(), error);
        if (!value) return std::nullopt;
        if (name == kStateMachineComponent && !machine((*value)["machine"].get<std::string>())) {
            error = std::format("\"{}\": \"machine\" {} names no valid state machine definition in {}/", name,
                                (*value)["machine"].dump(), kStateMachinesDir);
            return std::nullopt;
        }
        base[name] = std::move(*value);
    }
    return base;
}

NpcDefsResult load_npc_defs(sol::state& lua, const std::filesystem::path& root, const Defs& defs,
                            const MachineExists& machine) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const std::filesystem::path dir = root / kNpcsDir;
    if (std::filesystem::is_directory(dir, ec))
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".lua") files.push_back(it->path());
    std::ranges::sort(files);

    struct Parsed {
        std::string path, name, error; // name: as declared, even if the file was rejected later
        std::optional<NpcDef> def;
    };
    std::vector<Parsed> parsed;
    for (const auto& file : files) {
        Parsed p;
        p.path = std::filesystem::relative(file, root, ec).generic_string();
        try {
            p.def = parse_definition(lua, file, p.path, defs, machine, p.name);
        } catch (const Invalid& e) {
            p.error = e.what();
        } catch (const std::exception& e) { // sol2 conversions
            p.error = std::format("{}: {}", p.path, e.what());
        }
        parsed.push_back(std::move(p));
    }
    // Name clashes reject every file of a name, so the result doesn't depend on scan order.
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (!parsed[i].def) continue;
        std::string others;
        for (size_t j = 0; j < parsed.size(); ++j)
            if (j != i && parsed[j].name == parsed[i].def->name)
                others += std::format("{}{}", others.empty() ? "" : ", ", parsed[j].path);
        if (!others.empty())
            parsed[i].error = std::format("{}: name \"{}\" is also declared in {}", parsed[i].path, parsed[i].def->name, others);
    }
    NpcDefsResult out;
    for (Parsed& p : parsed) {
        if (p.error.empty()) out.defs.emplace(p.def->name, std::move(*p.def));
        else out.errors.push_back(std::move(p.error));
    }
    return out;
}

} // namespace kast::lua
