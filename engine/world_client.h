#pragma once

#include "auth_client.h"
#include "network.h"

#include <nlohmann/json.hpp>

#include <chrono>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// WorldClient: the game side of the world server protocol (docs/services/world/protocol.html) over a Network TLS connection,
// encoded and decoded by services/world/protocol (kast_protocol). Connects to a realm from the auth service's realm
// list with the AuthClient's access token, pings it (rttMs), answers its Pings, and turns every server message and the connection's life
// into "world.*" events (docs/engine/online-services.html#kastworld); a ChatNotice (a muted say) becomes "chat.error",
// as ChatClient's. One connection at a time. No Lua, no EventBus, no scene: LuaSystems owns
// one per Play session, routes the Network results and auth events to it and publishes what poll() returns. Never logs
// the access token; chat text only at debug level. Same threading rule as Network.
namespace kast {

enum class WorldState { Disconnected, Connecting, Lobby, InWorld };

const char* worldStateName(WorldState state); // "disconnected", "connecting", "lobby", "inWorld"

// Client round-trip time: while connected, a Ping every kInterval; the RTT is that of the newest answered one. The
// clock is passed in, so tests need no sleeping.
class PingTracker {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kInterval{1};
    static constexpr std::chrono::seconds kTimeout{5}; // unanswered this long: no RTT, and its late Pong is ignored

    // The nonce of the Ping to send now, if one is due. Forgets the Pings past kTimeout (so at most
    // kTimeout / kInterval + 1 are outstanding).
    std::optional<uint32_t> due(Clock::time_point now);
    void pong(uint32_t nonce, Clock::time_point now); // ignored for a nonce never sent or already forgotten
    // Whole ms; nullopt before the first Pong and while a Ping is unanswered past kTimeout.
    std::optional<int> rttMs(Clock::time_point now) const;
    size_t outstanding() const { return sent_.size(); }

private:
    uint32_t nonce_ = 0;
    std::optional<Clock::time_point> last_; // the last Ping's send time
    std::map<uint32_t, Clock::time_point> sent_; // unanswered nonce -> send time
    std::optional<int> rtt_;
    Clock::time_point rttSent_; // the send time of the Ping rtt_ measured
};

// "host:port" -> {host, port}; nullopt for anything else (port 1..65535).
std::optional<std::pair<std::string, uint16_t>> parseHostPort(const std::string& text);

class WorldClient {
public:
    // Every connection's Network name starts with this (then a serial number, so a late result of an old connection
    // never reaches a new one).
    static constexpr const char* kConnPrefix = "kast.world.";

    WorldClient(Network& network, AuthClient& auth);

    // Drops the connection (no event), the realm cache and queued events, then uses config's caPem to trust the world
    // server (nullopt: disabled). Keeps the address override.
    void reset(std::optional<AuthConfig> config = std::nullopt);
    bool enabled() const { return enabled_; }
    WorldState state() const { return state_; }
    // The connection's round-trip time in whole ms (PingTracker); nullopt while not connected.
    std::optional<int> rttMs() const;

    // Editor only: "host:port" replacing every realm's address (nullopt: use the realm's). Throws std::invalid_argument
    // if it isn't host:port. The shipped player never calls it.
    void setAddressOverride(std::optional<std::string> hostPort);
    const std::optional<std::string>& addressOverride() const { return override_; }

    // Requires enabled(). connect needs state Disconnected; the character calls Lobby; say and sendEvent InWorld. Otherwise they throw
    // std::logic_error (a script bug). Everything the server or network decides comes back as events.
    void connect(uint64_t realmId); // no session -> world.disconnected not_logged_in, no network use
    void disconnect();              // world.disconnected client_disconnect, unless already disconnected
    void listCharacters();
    // options: the game's creation choices (a class, ...), checked by the server; the engine only carries them.
    void createCharacter(const std::string& name, const std::map<std::string, std::string>& options = {});
    void deleteCharacter(uint64_t charId);
    void enterWorld(uint64_t charId);
    // kind: "say" (the players in view), "yell" (the map within the server's CHAT_YELL_RADIUS) or "emote" (shown as
    // "<name> <text>" in say range); another kind throws std::logic_error.
    void say(const std::string& text, const std::string& kind = "say");
    // ClientEvent{name, data as JSON} for the server's Lua ("client.<name>"). Checks the name (1-32 chars of
    // [a-z0-9._]) and the encoded size (<= 1 KB) first, then the state (InWorld); each throws std::logic_error.
    // data must be a JSON object.
    void sendEvent(const std::string& name, const nlohmann::json& data);
    // The same ClientEvent tagged with a new requestId (returned; never 0), which the server's Lua answers with
    // kast.reply: world.reply {requestId, ok = true, data} or {requestId, ok = false, reason}. No answer within
    // timeout: world.reply {requestId, ok = false, reason = "timeout"} (a late answer is then ignored); the connection
    // dropping: reason "disconnected", just before world.disconnected. Exactly one world.reply per request.
    uint32_t request(const std::string& name, const nlohmann::json& data,
                     std::chrono::milliseconds timeout = std::chrono::seconds(2));
    // Interact{objectId}: activates a zone object (an interactable's id). The server answers only through its Lua.
    void interact(uint32_t objectId);
    // Move{position, heading (radians, atan2(dx, dz)), clientTimeMs, seq, epoch}: seq and epoch are the caller's
    // (Replication's MoveHistory). A rejected move comes back as world.correction {position, ackSeq, epoch}.
    void move(float x, float y, float z, float heading, uint32_t seq, uint32_t epoch);

    // Takes a Network result of this client's connections (true), else leaves it alone (false).
    bool handle(const NetEvent& event);
    // Sees every AuthClient event before it is published: auth.realms results are the realm cache; the answer to a
    // realms request connect() made resolves that connect.
    void observe(const NetEvent& authEvent);
    // The events since the last poll, oldest first.
    std::vector<NetEvent> poll();
    // The last payload of each event this connection emitted (since world.connected; none after world.disconnected),
    // for kast.subscribe's replay: a scene loaded later catches up on what its scripts missed (a game's one-shot
    // server.* events; Replication itself tells a new scene world.entered and world.component again). Null if none.
    const nlohmann::json* latest(const std::string& name) const;

private:
    void require(WorldState state, const char* function) const;
    void open(const nlohmann::json& realm);
    void send(const auto& message);
    void received(const std::string& bytes);
    void dispatch(const auto& message);
    void drop(const std::string& code, const std::string& reason); // closes, world.disconnected {code, reason}
    void emit(std::string name, nlohmann::json payload);
    std::string eventJson(const std::string& name, const nlohmann::json& data, const char* function) const;
    void expireRequests(); // world.reply timeout for the requests past their deadline

    Network& network_;
    AuthClient& auth_;
    bool enabled_ = false;
    std::string caPem_;
    std::optional<std::string> override_;
    WorldState state_ = WorldState::Disconnected;
    std::string conn_;     // the current connection's Network name, empty if none
    uint64_t serial_ = 0;  // for conn_
    std::string inbox_;    // received bytes not yet a whole frame
    nlohmann::json realms_; // the last auth.realms list, null if none seen
    std::optional<uint64_t> pendingRealm_;   // connect() waiting for a realm list
    uint64_t realmsRequest_ = 0;             // the AuthClient call id of that list, 0 if it used the cache
    std::vector<NetEvent> events_;
    std::map<std::string, nlohmann::json> latest_; // event name -> its last payload, see latest()
    uint32_t requestSerial_ = 0;                   // the last requestId
    std::map<uint32_t, std::chrono::steady_clock::time_point> requests_; // unanswered requestId -> deadline
    PingTracker ping_; // reset with each connection
};

} // namespace kast
