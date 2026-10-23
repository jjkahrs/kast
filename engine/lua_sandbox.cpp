#include "lua_sandbox.h"

#include "files.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>
#include <vector>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

// Base-library functions that load code from disk or reach outside a file's environment.
constexpr const char* kUnsafeGlobals[] = {"dofile", "loadfile", "load", "loadstring", "getfenv", "setfenv", "require", "module"};
constexpr const char* kVectorKeys[] = {"x", "y", "z", "w"};
constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53: Lua numbers are doubles

constexpr int kHookInstructions = 10000; // between clock checks
using Clock = std::chrono::steady_clock;
Clock::time_point g_deadline = Clock::time_point::max(); // ponytail: Lua runs on one thread, one entry at a time

void watchdog(lua_State* L, lua_Debug*)
{
    if (Clock::now() < g_deadline)
        return;
    lua_sethook(L, watchdog, LUA_MASKCOUNT, 1); // every instruction until the entry point returns: pcall can't hide it
    luaL_where(L, 0); // in a hook, level 0 is the running Lua function
    lua_pushfstring(L, "script ran longer than %d seconds (infinite loop?)", static_cast<int>(kScriptBudget.count()));
    lua_concat(L, 2);
    lua_error(L);
}

// Integral and within double's exact range: stored as a JSON integer.
bool exactInteger(double number) { return number == std::floor(number) && std::abs(number) <= kMaxExactInteger; }

size_t vectorSize(FieldKind kind) { return kind == FieldKind::Vec3 ? 3 : kind == FieldKind::Quat ? 4 : 0; }

// label names the value in errors: "\"f\"" or "\"f\" element 2".
bool scalarToJson(FieldKind kind, const std::string& label, const sol::object& value, Json& out, std::string& error)
{
    if (const size_t count = vectorSize(kind)) {
        out = Json::array();
        if (value.get_type() == sol::type::table) {
            const sol::table table = value.as<sol::table>();
            for (size_t i = 0; i < count; ++i) {
                const sol::object c = table[kVectorKeys[i]];
                if (c.get_type() == sol::type::number && std::isfinite(c.as<double>()))
                    out.push_back(c.as<double>());
            }
        }
        if (out.size() == count)
            return true;
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
        if (exactInteger(number))
            out = static_cast<int64_t>(number); // an integer, so int fields accept it
        else
            out = number;
        return true;
    }
    case sol::type::boolean: out = value.as<bool>(); return true;
    case sol::type::string: out = value.as<std::string>(); return true;
    default: out = nullptr; return true; // rejected by the JSON checks with the field's expected type
    }
}

sol::object scalarToLua(sol::state_view lua, FieldKind kind, const Json& value)
{
    if (const size_t count = vectorSize(kind)) {
        sol::table table = lua.create_table();
        for (size_t i = 0; i < count; ++i)
            table[kVectorKeys[i]] = value[i].get<double>();
        return table;
    }
    if (value.is_boolean())
        return sol::make_object(lua, value.get<bool>());
    if (value.is_number())
        return sol::make_object(lua, value.get<double>());
    return sol::make_object(lua, value.get<std::string>()); // strings and enum names
}

// label names the value in errors: "payload", "payload.id", "payload[2]". path holds the tables being converted,
// outermost first, to catch cycles.
bool payloadToJson(const sol::object& value, const std::string& label, std::vector<const void*>& path,
                   nlohmann::json& out, std::string& error)
{
    switch (value.get_type()) {
    case sol::type::lua_nil:
    case sol::type::none: out = nullptr; return true;
    case sol::type::boolean: out = value.as<bool>(); return true;
    case sol::type::string: out = value.as<std::string>(); return true;
    case sol::type::number: {
        const double number = value.as<double>();
        if (!std::isfinite(number)) {
            error = std::format("{} must be a finite number", label);
            return false;
        }
        if (exactInteger(number))
            out = static_cast<int64_t>(number);
        else
            out = number;
        return true;
    }
    case sol::type::table: break;
    default:
        error = std::format("{}: a {} can't be sent in an event (only nil, booleans, numbers, strings and tables)",
                            label, sol::type_name(value.lua_state(), value.get_type()));
        return false;
    }
    const sol::table table = value.as<sol::table>();
    if (std::ranges::find(path, table.pointer()) != path.end()) {
        error = std::format("{}: the table contains itself (cycle)", label);
        return false;
    }
    if (path.size() >= static_cast<size_t>(kMaxPayloadDepth)) {
        error = std::format("{}: tables are nested more than {} deep", label, kMaxPayloadDepth);
        return false;
    }
    // Keys exactly 1..n: n distinct positive integers whose largest is n.
    size_t count = 0, strings = 0;
    double largest = 0.0;
    bool positiveIntegers = true;
    for (const auto& [key, unused] : table) {
        ++count;
        if (key.get_type() == sol::type::string) {
            ++strings;
            positiveIntegers = false;
            continue;
        }
        const double k = key.get_type() == sol::type::number ? key.as<double>() : 0.0;
        positiveIntegers = positiveIntegers && k >= 1.0 && k == std::floor(k);
        largest = std::max(largest, k);
    }
    path.push_back(table.pointer());
    bool ok = true;
    if (count > 0 && positiveIntegers && largest == static_cast<double>(count)) {
        out = nlohmann::json::array();
        for (size_t i = 1; ok && i <= count; ++i)
            ok = payloadToJson(table[i], std::format("{}[{}]", label, i), path, out.emplace_back(), error);
    } else if (strings == count) { // also the empty table
        out = nlohmann::json::object();
        for (const auto& [key, item] : table) {
            const std::string name = key.as<std::string>();
            if (!(ok = payloadToJson(item, std::format("{}.{}", label, name), path, out[name], error)))
                break;
        }
    } else {
        error = std::format("{}: table keys must be exactly 1..n (an array) or all strings (an object)", label);
        ok = false;
    }
    path.pop_back();
    return ok;
}

} // namespace

void openSandbox(sol::state& lua)
{
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    for (const char* name : kUnsafeGlobals)
        lua.globals()[name] = sol::lua_nil;
    // Every string shares one metatable whose __index is the real string table: hide it so no file can edit the
    // methods (s:format()) every other file uses. Method calls still work.
    lua_pushliteral(lua.lua_state(), "");
    lua_getmetatable(lua.lua_state(), -1);
    lua_pushboolean(lua.lua_state(), 0);
    lua_setfield(lua.lua_state(), -2, "__metatable");
    lua_pop(lua.lua_state(), 2);
    lua_sethook(lua.lua_state(), watchdog, LUA_MASKCOUNT, kHookInstructions);
}

ScriptBudget::ScriptBudget(lua_State* L) : L_(L), outer_(g_deadline == Clock::time_point::max())
{
    if (outer_)
        g_deadline = Clock::now() + kScriptBudget;
}

ScriptBudget::~ScriptBudget()
{
    if (!outer_)
        return;
    g_deadline = Clock::time_point::max();
    lua_sethook(L_, watchdog, LUA_MASKCOUNT, kHookInstructions);
}

sol::environment sandboxEnvironment(sol::state& lua)
{
    sol::environment env(lua, sol::create);
    for (const auto& [key, value] : lua.globals()) {
        if (value.get_type() != sol::type::table) {
            env[key] = value;
            continue;
        }
        sol::table copy = lua.create_table();
        for (const auto& [k, v] : value.as<sol::table>())
            copy[k] = v;
        env[key] = copy;
    }
    env["_G"] = env;
    return env;
}

sol::protected_function loadSandboxed(sol::state& lua, const std::filesystem::path& file, const std::string& chunkName,
                                      const sol::environment& env, std::string& error)
{
    std::string text;
    if (!readFile(file, text)) {
        error = std::format("cannot open '{}'", file.string());
        return {};
    }
    sol::load_result chunk = lua.load(text, chunkName, sol::load_mode::text);
    if (!chunk.valid()) {
        error = chunk.get<sol::error>().what();
        return {};
    }
    sol::protected_function body = chunk;
    sol::set_environment(env, body);
    return body;
}

void raise(lua_State* L, const std::string& message)
{
    luaL_where(L, 1);
    std::string where = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error(where + message);
}

std::string toString(lua_State* L, const sol::object& value)
{
    sol::protected_function tostring = sol::state_view(L)["tostring"];
    sol::protected_function_result result = tostring(value);
    return result.valid() && result.get_type() == sol::type::string ? result.get<std::string>() : "?";
}

void checkKeys(const sol::table& table, std::initializer_list<const char*> allowed, const std::string& what)
{
    for (const auto& [key, value] : table) {
        const bool known = key.get_type() == sol::type::string &&
                           std::ranges::any_of(allowed, [&](const char* k) { return key.as<std::string>() == k; });
        if (!known)
            throw InvalidDefinition(std::format("{}unknown key {}", what, toString(table.lua_state(), key)));
    }
}

void checkPlainData(const sol::table& table, const std::string& what)
{
    lua_State* L = table.lua_state();
    std::unordered_set<const void*> seen;
    std::vector<sol::table> pending{table}; // not recursive: nesting depth is up to the file
    while (!pending.empty()) {
        const sol::table next = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(next.pointer()).second)
            continue;
        next.push();
        const int hasMeta = lua_getmetatable(L, -1);
        lua_pop(L, hasMeta ? 2 : 1);
        if (hasMeta)
            throw InvalidDefinition(what + "tables must be plain data (no metatables)");
        for (const auto& [key, value] : next) // lua_next: raw, runs no metamethods
            for (const sol::object& item : {key, value})
                if (item.get_type() == sol::type::table)
                    pending.push_back(item.as<sol::table>());
    }
}

std::vector<sol::object> sequence(const sol::object& value, const std::string& what)
{
    if (value.get_type() != sol::type::table)
        throw InvalidDefinition(std::format("{} must be a list {{...}}", what));
    const sol::table table = value.as<sol::table>();
    size_t count = 0;
    for (const auto& [key, unused] : table)
        ++count;
    std::vector<sol::object> items;
    for (size_t i = 1; i <= count; ++i) {
        sol::object item = table[i];
        if (item.get_type() == sol::type::lua_nil)
            throw InvalidDefinition(std::format("{} must be a list {{...}}", what));
        items.push_back(item);
    }
    return items;
}

bool luaToFieldJson(const FieldDesc& field, const sol::object& value, Json& out, std::string& error, bool element)
{
    const std::string label = std::format("\"{}\"", field.name);
    if (!field.array || element)
        return scalarToJson(field.kind, label, value, out, error);
    // A sequence: exactly the integer keys 1..n.
    size_t count = 0;
    bool sequence = value.get_type() == sol::type::table;
    if (sequence) {
        for (const auto& [key, unused] : value.as<sol::table>()) {
            ++count;
            const double k = key.get_type() == sol::type::number ? key.as<double>() : 0.0;
            sequence = sequence && k >= 1.0 && k == std::floor(k);
        }
        for (size_t i = 1; sequence && i <= count; ++i) // count positive integer keys including 1..count: exactly those
            sequence = value.as<sol::table>()[i].get_type() != sol::type::lua_nil;
    }
    if (!sequence) {
        error = std::format("{} must be a sequence table {{v1, v2, ...}}", label);
        return false;
    }
    const sol::table table = value.as<sol::table>();
    out = Json::array();
    for (size_t i = 1; i <= count; ++i) {
        Json item;
        if (!scalarToJson(field.kind, std::format("{} element {}", label, i), table[i], item, error))
            return false;
        out.push_back(std::move(item));
    }
    return true;
}

sol::object fieldJsonToLua(sol::state_view lua, const FieldDesc& field, const Json& value, bool element)
{
    if (!field.array || element)
        return scalarToLua(lua, field.kind, value);
    sol::table table = lua.create_table();
    for (size_t i = 0; i < value.size(); ++i)
        table[i + 1] = scalarToLua(lua, field.kind, value[i]);
    return table;
}

bool luaToJson(const sol::object& value, nlohmann::json& out, std::string& error, const std::string& label)
{
    std::vector<const void*> path;
    return payloadToJson(value, label, path, out, error);
}

sol::object jsonToLua(sol::state_view lua, const nlohmann::json& value)
{
    switch (value.type()) {
    case nlohmann::json::value_t::boolean: return sol::make_object(lua, value.get<bool>());
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
    case nlohmann::json::value_t::number_float: return sol::make_object(lua, value.get<double>());
    case nlohmann::json::value_t::string: return sol::make_object(lua, value.get_ref<const std::string&>());
    case nlohmann::json::value_t::array: {
        sol::table table = lua.create_table(static_cast<int>(value.size()), 0);
        for (size_t i = 0; i < value.size(); ++i)
            table[i + 1] = jsonToLua(lua, value[i]);
        return table;
    }
    case nlohmann::json::value_t::object: {
        sol::table table = lua.create_table(0, static_cast<int>(value.size()));
        for (const auto& [key, item] : value.items())
            table[key] = jsonToLua(lua, item);
        return table;
    }
    default: return sol::make_object(lua, sol::lua_nil); // null (and discarded)
    }
}

} // namespace kast
