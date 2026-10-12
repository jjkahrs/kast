#include "lua_sandbox.h"

#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

// Base-library functions that load code from disk or reach outside a file's environment.
constexpr const char* kUnsafeGlobals[] = {"dofile", "loadfile", "load", "loadstring", "getfenv", "setfenv", "require", "module"};
constexpr const char* kVectorKeys[] = {"x", "y", "z", "w"};
constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53: Lua numbers are doubles

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
        if (number == std::floor(number) && std::abs(number) <= kMaxExactInteger)
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

} // namespace

void openSandbox(sol::state& lua)
{
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    for (const char* name : kUnsafeGlobals)
        lua.globals()[name] = sol::lua_nil;
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

std::string toString(lua_State* L, const sol::object& value)
{
    sol::protected_function tostring = sol::state_view(L)["tostring"];
    sol::protected_function_result result = tostring(value);
    return result.valid() && result.get_type() == sol::type::string ? result.get<std::string>() : "?";
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

} // namespace kast
