#pragma once

#include "auth_client.h"
#include "network.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// WorldClient: the game side of the world server protocol (docs/services/world/protocol.html) over a Network TLS connection,
// encoded and decoded by services/world/protocol (kast_protocol). Connects to a realm from the auth service's realm
// list with the AuthClient's access token, answers Ping, and turns every server message and the connection's life
// into "world.*" events (docs/engine/online-services.html#kastworld). One connection at a time. No Lua, no EventBus, no scene: LuaSystems owns
// one per Play session, routes the Network results and auth events to it and publishes what poll() returns. Never logs
// the access token; chat text only at debug level. Same threading rule as Network.
namespace kast {

enum class WorldState { Disconnected, Connecting, Lobby, InWorld };

const char* worldStateName(WorldState state); // "disconnected", "connecting", "lobby", "inWorld"

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
    void say(const std::string& text);
    // ClientEvent{name, data as JSON} for the server's Lua ("client.<name>"). Checks the name (1-32 chars of
    // [a-z0-9._]) and the encoded size (<= 1 KB) first, then the state (InWorld); each throws std::logic_error.
    // data must be a JSON object.
    void sendEvent(const std::string& name, const nlohmann::json& data);
    // Interact{objectId}: activates a zone object (an interactable's id). The server answers only through its Lua.
    void interact(uint32_t objectId);
    // Move{position, heading (radians, atan2(dx, dz)), clientTimeMs}. Rejected moves come back as world.correction.
    void move(float x, float y, float z, float heading);

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
};

} // namespace kast
