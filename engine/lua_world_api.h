#pragma once

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace kast {

class WorldClient;

// kast.world.*: thin wrappers over WorldClient; a bad argument or a call in the wrong state raises at the calling line,
// everything the server or network decides is a world.* event. See docs/engine/online-services.html.
// kast.world.request is LuaSystems' (its callbacks belong to the calling system).
sol::table makeWorldApi(sol::state& lua, WorldClient& world);

// kast.world.send/request's data (a table with string keys, nil: {}) as a JSON object; raises "<function>: ...".
nlohmann::json worldEventData(lua_State* L, const char* function, const sol::object& data);

} // namespace kast
