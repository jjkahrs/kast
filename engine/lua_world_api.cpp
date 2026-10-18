#include "lua_world_api.h"

#include "lua_sandbox.h"
#include "world_client.h"

#include <cmath>
#include <format>
#include <stdexcept>
#include <string>

namespace kast {

sol::table makeWorldApi(sol::state& lua, WorldClient& world)
{
    const auto guarded = [](lua_State* L, auto&& body) {
        try {
            return body();
        } catch (const std::logic_error& e) { // WorldClient's messages start with the function's name
            raise(L, std::format("kast.world.{}", e.what()));
        }
    };
    // ids are numbers in Lua (accountId, charId): whole, 0 .. 2^53
    const auto id = [](lua_State* L, const char* function, const char* what, const sol::object& value) {
        const double n = value.get_type() == sol::type::number ? value.as<double>() : -1.0;
        if (n < 0 || n > 9007199254740992.0 || n != std::floor(n))
            raise(L, std::format("kast.world.{}: {} must be a whole number >= 0, got {}", function, what,
                                 toString(L, value)));
        return static_cast<uint64_t>(n);
    };
    const auto text = [](lua_State* L, const char* function, const char* what, const sol::object& value) {
        if (value.get_type() != sol::type::string)
            raise(L, std::format("kast.world.{}: {} must be a string, got {}", function, what, toString(L, value)));
        return value.as<std::string>();
    };
    sol::table worldApi = lua.create_table();
    worldApi.set_function("connect", [=, &world](sol::this_state L, const sol::object& realmId) {
        const uint64_t realm = id(L, "connect", "realmId", realmId);
        guarded(L, [&] { world.connect(realm); });
    });
    worldApi.set_function("disconnect", [&world]() { world.disconnect(); });
    worldApi.set_function("listCharacters", [=, &world](sol::this_state L) { guarded(L, [&] { world.listCharacters(); }); });
    worldApi.set_function("createCharacter", [=, &world](sol::this_state L, const sol::object& name) {
        const std::string n = text(L, "createCharacter", "name", name);
        guarded(L, [&] { world.createCharacter(n); });
    });
    worldApi.set_function("deleteCharacter", [=, &world](sol::this_state L, const sol::object& charId) {
        const uint64_t c = id(L, "deleteCharacter", "charId", charId);
        guarded(L, [&] { world.deleteCharacter(c); });
    });
    worldApi.set_function("enterWorld", [=, &world](sol::this_state L, const sol::object& charId) {
        const uint64_t c = id(L, "enterWorld", "charId", charId);
        guarded(L, [&] { world.enterWorld(c); });
    });
    worldApi.set_function("say", [=, &world](sol::this_state L, const sol::object& message) {
        const std::string t = text(L, "say", "text", message);
        guarded(L, [&] { world.say(t); });
    });
    // kast.world.send(name, data?): data is a table with string keys (nil: {}); "client.<name>" on the server's bus.
    worldApi.set_function("send", [=, &world](sol::this_state L, const sol::object& name, const sol::object& data) {
        const std::string n = text(L, "send", "name", name);
        nlohmann::json json = nlohmann::json::object();
        if (data.get_type() != sol::type::lua_nil) {
            if (data.get_type() != sol::type::table)
                raise(L, std::format("kast.world.send: data must be a table, got {}", toString(L, data)));
            if (std::string error; !luaToJson(data, json, error))
                raise(L, std::format("kast.world.send: {}", error));
            if (json.is_array() && json.empty()) // {} reads as an empty list
                json = nlohmann::json::object();
            if (!json.is_object())
                raise(L, "kast.world.send: data must be a table with string keys, not a list");
        }
        guarded(L, [&] { world.sendEvent(n, json); });
    });
    worldApi.set_function("state", [&world]() { return std::string(worldStateName(world.state())); });
    return worldApi;
}

} // namespace kast
