#include "sandbox.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace kast::lua {
namespace {

constexpr const char* kUnsafeGlobals[] = {"dofile", "loadfile", "load", "loadstring", "getfenv", "setfenv", "require", "module"};
constexpr const char* kVectorKeys[] = {"x", "y", "z", "w"};
constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53: Lua numbers are doubles

constexpr int kHookInstructions = 10000; // between clock checks
using Clock = std::chrono::steady_clock;
Clock::time_point g_deadline = Clock::time_point::max(); // ponytail: Lua runs on the tick thread only, one entry at a time

void watchdog(lua_State* L, lua_Debug*) {
    if (Clock::now() < g_deadline) return;
    lua_sethook(L, watchdog, LUA_MASKCOUNT, 1); // every instruction until the entry point returns: pcall can't hide it
    luaL_where(L, 0);
    lua_pushfstring(L, "script ran longer than %d seconds (infinite loop?)", static_cast<int>(kScriptBudget.count()));
    lua_concat(L, 2);
    lua_error(L);
}

bool exact_integer(double number) { return number == std::floor(number) && std::abs(number) <= kMaxExactInteger; }

size_t vector_size(FieldKind kind) { return kind == FieldKind::Vec3 ? 3 : kind == FieldKind::Quat ? 4 : 0; }

bool scalar_to_json(FieldKind kind, const std::string& label, const sol::object& value, Json& out, std::string& error) {
    if (const size_t count = vector_size(kind)) {
        out = Json::array();
        if (value.get_type() == sol::type::table) {
            const sol::table table = value.as<sol::table>();
            for (size_t i = 0; i < count; ++i) {
                const sol::object c = table[kVectorKeys[i]];
                if (c.get_type() == sol::type::number && std::isfinite(c.as<double>())) out.push_back(c.as<double>());
            }
        }
        if (out.size() == count) return true;
        error = std::format("{} must be a table {{x=, y=, z={}}} of finite numbers", label, count == 4 ? ", w=" : "");
        return false;
    }
    switch (value.get_type()) {
    case sol::type::number: {
        const double number = value.as<double>();
        if (!std::isfinite(number)) {
            error = std::format("{} must be a finite number", label);
            return false;
        }
        if (exact_integer(number)) out = static_cast<int64_t>(number); // an integer, so int fields accept it
        else out = number;
        return true;
    }
    case sol::type::boolean: out = value.as<bool>(); return true;
    case sol::type::string: out = value.as<std::string>(); return true;
    default: out = nullptr; return true; // rejected by the JSON checks with the field's expected type
    }
}

sol::object scalar_to_lua(sol::state_view lua, FieldKind kind, const Json& value) {
    if (const size_t count = vector_size(kind)) {
        sol::table table = lua.create_table();
        for (size_t i = 0; i < count; ++i) table[kVectorKeys[i]] = value[i].get<double>();
        return table;
    }
    if (value.is_boolean()) return sol::make_object(lua, value.get<bool>());
    if (value.is_number()) return sol::make_object(lua, value.get<double>());
    return sol::make_object(lua, value.get<std::string>()); // strings and enum names
}

} // namespace

void open_sandbox(sol::state& lua) {
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    for (const char* name : kUnsafeGlobals) lua.globals()[name] = sol::lua_nil;
    // Every string shares one metatable whose __index is the real string table: hide it so no file can edit the
    // methods every other file uses. Method calls still work.
    lua_pushliteral(lua.lua_state(), "");
    lua_getmetatable(lua.lua_state(), -1);
    lua_pushboolean(lua.lua_state(), 0);
    lua_setfield(lua.lua_state(), -2, "__metatable");
    lua_pop(lua.lua_state(), 2);
    lua_sethook(lua.lua_state(), watchdog, LUA_MASKCOUNT, kHookInstructions);
}

ScriptBudget::ScriptBudget(lua_State* L) : L_(L), outer_(g_deadline == Clock::time_point::max()) {
    if (outer_) g_deadline = Clock::now() + kScriptBudget;
}

ScriptBudget::~ScriptBudget() {
    if (!outer_) return;
    g_deadline = Clock::time_point::max();
    lua_sethook(L_, watchdog, LUA_MASKCOUNT, kHookInstructions);
}

sol::environment sandbox_environment(sol::state& lua) {
    sol::environment env(lua, sol::create);
    for (const auto& [key, value] : lua.globals()) {
        if (value.get_type() != sol::type::table) {
            env[key] = value;
            continue;
        }
        sol::table copy = lua.create_table();
        for (const auto& [k, v] : value.as<sol::table>()) copy[k] = v;
        env[key] = copy;
    }
    env["_G"] = env;
    return env;
}

sol::protected_function load_sandboxed(sol::state& lua, const std::filesystem::path& file, const std::string& chunkName,
                                       const sol::environment& env, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = std::format("cannot open '{}'", file.string());
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    sol::load_result chunk = lua.load(text.str(), chunkName, sol::load_mode::text);
    if (!chunk.valid()) {
        error = chunk.get<sol::error>().what();
        return {};
    }
    sol::protected_function body = chunk;
    sol::set_environment(env, body);
    return body;
}

std::string to_string(lua_State* L, const sol::object& value) {
    sol::protected_function tostring = sol::state_view(L)["tostring"];
    sol::protected_function_result result = tostring(value);
    return result.valid() && result.get_type() == sol::type::string ? result.get<std::string>() : "?";
}

void check_keys(const sol::table& table, std::initializer_list<const char*> allowed, const std::string& what) {
    for (const auto& [key, value] : table) {
        const bool known = key.get_type() == sol::type::string &&
                           std::ranges::any_of(allowed, [&](const char* k) { return key.as<std::string>() == k; });
        if (!known) throw InvalidDefinition(std::format("{}unknown key {}", what, to_string(table.lua_state(), key)));
    }
}

void check_plain_data(const sol::table& table, const std::string& what) {
    lua_State* L = table.lua_state();
    std::unordered_set<const void*> seen;
    std::vector<sol::table> pending{table}; // not recursive: nesting depth is up to the file
    while (!pending.empty()) {
        const sol::table next = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(next.pointer()).second) continue;
        next.push();
        const int hasMeta = lua_getmetatable(L, -1);
        lua_pop(L, hasMeta ? 2 : 1);
        if (hasMeta) throw InvalidDefinition(what + "tables must be plain data (no metatables)");
        for (const auto& [key, value] : next) // lua_next: raw, runs no metamethods
            for (const sol::object& item : {key, value})
                if (item.get_type() == sol::type::table) pending.push_back(item.as<sol::table>());
    }
}

std::vector<sol::object> sequence(const sol::object& value, const std::string& what) {
    if (value.get_type() != sol::type::table) throw InvalidDefinition(std::format("{} must be a list {{...}}", what));
    const sol::table table = value.as<sol::table>();
    size_t count = 0;
    for (auto it = table.begin(); it != table.end(); ++it) ++count;
    std::vector<sol::object> items;
    for (size_t i = 1; i <= count; ++i) {
        sol::object item = table[i];
        if (item.get_type() == sol::type::lua_nil) throw InvalidDefinition(std::format("{} must be a list {{...}}", what));
        items.push_back(item);
    }
    return items;
}

bool lua_to_field_json(const FieldDesc& field, const sol::object& value, Json& out, std::string& error, bool element) {
    const std::string label = std::format("\"{}\"", field.name);
    if (!field.array || element) return scalar_to_json(field.kind, label, value, out, error);
    size_t count = 0;
    bool isSequence = value.get_type() == sol::type::table;
    if (isSequence) {
        for (const auto& [key, unused] : value.as<sol::table>()) {
            ++count;
            const double k = key.get_type() == sol::type::number ? key.as<double>() : 0.0;
            isSequence = isSequence && k >= 1.0 && k == std::floor(k);
        }
        for (size_t i = 1; isSequence && i <= count; ++i)
            isSequence = value.as<sol::table>()[i].get_type() != sol::type::lua_nil;
    }
    if (!isSequence) {
        error = std::format("{} must be a sequence table {{v1, v2, ...}}", label);
        return false;
    }
    const sol::table table = value.as<sol::table>();
    out = Json::array();
    for (size_t i = 1; i <= count; ++i) {
        Json item;
        if (!scalar_to_json(field.kind, std::format("{} element {}", label, i), table[i], item, error)) return false;
        out.push_back(std::move(item));
    }
    return true;
}

sol::object field_json_to_lua(sol::state_view lua, const FieldDesc& field, const Json& value, bool element) {
    if (!field.array || element) return scalar_to_lua(lua, field.kind, value);
    sol::table table = lua.create_table();
    for (size_t i = 0; i < value.size(); ++i) table[i + 1] = scalar_to_lua(lua, field.kind, value[i]);
    return table;
}

} // namespace kast::lua
