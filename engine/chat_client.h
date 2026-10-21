#pragma once

#include "auth_client.h"
#include "network.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// ChatClient: the game side of the realm's chat service (docs/services/chat/protocol.html), a second TLS connection
// next to WorldClient's, framed by services/world/protocol (kast_protocol). It connects where the world server's
// ChatServer said ("world.chatServer", seen through observe) with the AuthClient's current access token, and keeps
// reconnecting with backoff (1 s, doubling to 30 s) while the world connection lasts; "world.disconnected" ends it.
// After every (re)connect the service puts it in the first channel (General) and it joins the other channels it was
// in again. Turns the service's messages into "chat.*" events (docs/engine/online-services.html#kastchat). No Lua, no
// EventBus: LuaSystems owns one per Play session. Never logs the access token or chat text. Same threading rule as
// Network.
namespace kast {

enum class ChatState { Disconnected, Connecting, Connected };

const char* chatStateName(ChatState state); // "disconnected", "connecting", "connected"

class ChatClient {
public:
    static constexpr const char* kConnPrefix = "kast.chat."; // then a serial number, as WorldClient's
    static constexpr std::chrono::seconds kRetryMin{1}, kRetryMax{30};

    ChatClient(Network& network, AuthClient& auth);

    // Drops the connection (no event), the target and the remembered channels, then uses config's caPem to trust
    // the chat service (nullopt: disabled).
    void reset(std::optional<AuthConfig> config = std::nullopt);
    bool enabled() const { return enabled_; }
    ChatState state() const { return state_; }
    // The channels it is in: number (CHAT_CHANNELS order, from 1) -> name.
    const std::map<int, std::string>& channels() const { return channels_; }

    // Each needs state Connected (else std::logic_error, a script bug, message starting with the function's name).
    // What the service decides comes back as events.
    void whisper(const std::string& name, const std::string& text);
    // channel: a channel's name (channelName maps a number to it).
    void sendChannel(const std::string& channel, const std::string& text);
    void zone(const std::string& text);
    void join(const std::string& channel);
    void leave(const std::string& channel);
    void ignore(const std::string& name, bool on);
    void mute(const std::string& name, uint32_t minutes); // 0: unmute; GM / admin only (the service checks)
    // The name of the channel numbered n that it is in; std::logic_error if none.
    std::string channelName(int n, const char* function) const;

    // WorldClient's events, before they are published: world.chatServer {host, port} (re)starts the connection,
    // world.disconnected ends it.
    void observe(const NetEvent& worldEvent);
    // Takes a Network result of this client's connections (true), else leaves it alone (false).
    bool handle(const NetEvent& event);
    // Opens a due reconnect, then returns the events since the last poll, oldest first.
    std::vector<NetEvent> poll();

private:
    void require(const char* function) const;
    void open();
    void send(const auto& message);
    void received(const std::string& bytes);
    void dispatch(const auto& message);
    // Closes; chat.status {connected = false, reason} unless that was already the last status; retries later while
    // the world connection lasts.
    void drop(const std::string& reason);
    void emit(std::string name, nlohmann::json payload);
    void status(bool connected, const std::string& reason);

    Network& network_;
    AuthClient& auth_;
    bool enabled_ = false;
    std::string caPem_;
    ChatState state_ = ChatState::Disconnected;
    std::optional<std::pair<std::string, uint16_t>> target_; // from world.chatServer; none: not in a world
    std::string conn_;
    uint64_t serial_ = 0;
    std::string inbox_;
    std::map<int, std::string> channels_;  // this connection's
    std::set<std::string> wanted_;         // channels to be in again after a reconnect
    std::chrono::steady_clock::duration backoff_ = kRetryMin;
    std::optional<std::chrono::steady_clock::time_point> retryAt_;
    std::optional<bool> lastStatus_; // the last chat.status sent
    std::vector<NetEvent> events_;
};

} // namespace kast
