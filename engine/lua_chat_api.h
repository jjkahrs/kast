#pragma once

#include <sol/sol.hpp>

namespace kast {

class ChatClient;

// kast.chat.*: thin wrappers over ChatClient, as kast.world's over WorldClient: a bad argument or a call while the chat
// service isn't connected raises at the calling line; what the service decides is a chat.* event. See
// docs/engine/online-services.html#kastchat.
sol::table makeChatApi(sol::state& lua, ChatClient& chat);

} // namespace kast
