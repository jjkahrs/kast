#include "lua_chat_api.h"

#include "chat_client.h"
#include "lua_sandbox.h"

#include <cmath>
#include <format>
#include <stdexcept>
#include <string>

namespace kast {

sol::table makeChatApi(sol::state& lua, ChatClient& chat)
{
    const auto guarded = [](lua_State* L, auto&& body) {
        try {
            return body();
        } catch (const std::logic_error& e) { // ChatClient's messages start with the function's name
            raise(L, std::format("kast.chat.{}", e.what()));
        }
    };
    const auto text = [](lua_State* L, const char* function, const char* what, const sol::object& value) {
        if (value.get_type() != sol::type::string)
            raise(L, std::format("kast.chat.{}: {} must be a string, got {}", function, what, toString(L, value)));
        return value.as<std::string>();
    };
    sol::table api = lua.create_table();
    api.set_function("state", [&chat]() { return std::string(chatStateName(chat.state())); });
    // {{number = 1, name = "General"}, ...}: the channels it is in, by number.
    api.set_function("channels", [&chat](sol::this_state L) {
        sol::state_view lua(L);
        sol::table list = lua.create_table();
        for (const auto& [n, name] : chat.channels())
            list.add(lua.create_table_with("number", n, "name", name));
        return list;
    });
    api.set_function("whisper", [=, &chat](sol::this_state L, const sol::object& name, const sol::object& message) {
        const std::string n = text(L, "whisper", "name", name), t = text(L, "whisper", "text", message);
        guarded(L, [&] { chat.whisper(n, t); });
    });
    // kast.chat.send(channel, text): channel is a name or the number of a channel it is in.
    api.set_function("send", [=, &chat](sol::this_state L, const sol::object& channel, const sol::object& message) {
        const std::string t = text(L, "send", "text", message);
        guarded(L, [&] {
            if (channel.get_type() == sol::type::number) {
                const double n = channel.as<double>();
                if (n != std::floor(n) || n < 1 || n > 255)
                    raise(L, std::format("kast.chat.send: channel number must be a whole number 1-255, got {}", toString(L, channel)));
                return chat.sendChannel(chat.channelName(static_cast<int>(n), "send"), t);
            }
            chat.sendChannel(text(L, "send", "channel", channel), t);
        });
    });
    api.set_function("zone", [=, &chat](sol::this_state L, const sol::object& message) {
        const std::string t = text(L, "zone", "text", message);
        guarded(L, [&] { chat.zone(t); });
    });
    api.set_function("join", [=, &chat](sol::this_state L, const sol::object& channel) {
        const std::string c = text(L, "join", "channel", channel);
        guarded(L, [&] { chat.join(c); });
    });
    api.set_function("leave", [=, &chat](sol::this_state L, const sol::object& channel) {
        const std::string c = text(L, "leave", "channel", channel);
        guarded(L, [&] { chat.leave(c); });
    });
    api.set_function("ignore", [=, &chat](sol::this_state L, const sol::object& name) {
        const std::string n = text(L, "ignore", "name", name);
        guarded(L, [&] { chat.ignore(n, true); });
    });
    api.set_function("unignore", [=, &chat](sol::this_state L, const sol::object& name) {
        const std::string n = text(L, "unignore", "name", name);
        guarded(L, [&] { chat.ignore(n, false); });
    });
    // kast.chat.mute(name, minutes): GM / admin only (the service answers chat.error not_allowed otherwise).
    api.set_function("mute", [=, &chat](sol::this_state L, const sol::object& name, const sol::object& minutes) {
        const std::string n = text(L, "mute", "name", name);
        const double m = minutes.get_type() == sol::type::number ? minutes.as<double>() : 0.0;
        if (m < 1 || m > 4294967295.0 || m != std::floor(m))
            raise(L, std::format("kast.chat.mute: minutes must be a whole number >= 1, got {}", toString(L, minutes)));
        guarded(L, [&] { chat.mute(n, static_cast<uint32_t>(m)); });
    });
    api.set_function("unmute", [=, &chat](sol::this_state L, const sol::object& name) {
        const std::string n = text(L, "unmute", "name", name);
        guarded(L, [&] { chat.mute(n, 0); });
    });
    return api;
}

} // namespace kast
