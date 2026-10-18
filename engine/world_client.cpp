#include "world_client.h"

#include "log.h"

#include <codec.h>

#include <charconv>
#include <chrono>
#include <format>
#include <stdexcept>

namespace kast {
namespace {

using Json = nlohmann::json;
namespace proto = kast::proto;

// Server frames are never refused for size: the u16 length already caps them (a CharList can pass the server's
// own 4096-byte limit for client frames).
constexpr size_t kMaxPayload = 0xFFFF;

Json position(const proto::Vec3& v) { return {{"x", v.x}, {"y", v.y}, {"z", v.z}}; }

Json character(const proto::CharInfo& c)
{
    return {{"id", c.id}, {"name", c.name}, {"position", position(c.pos)}, {"options", c.options}};
}

const char* roleName(proto::Role role)
{
    switch (role) {
    case proto::Role::player: return "player";
    case proto::Role::gm: return "gm";
    case proto::Role::admin: return "admin";
    }
    return "unknown";
}

const Json* findRealm(const Json& realms, uint64_t id)
{
    if (!realms.is_array())
        return nullptr;
    for (const Json& realm : realms)
        if (realm.is_object() && realm.value("id", uint64_t{0}) == id)
            return &realm;
    return nullptr;
}

} // namespace

const char* worldStateName(WorldState state)
{
    switch (state) {
    case WorldState::Disconnected: return "disconnected";
    case WorldState::Connecting: return "connecting";
    case WorldState::Lobby: return "lobby";
    case WorldState::InWorld: return "inWorld";
    }
    return "disconnected";
}

std::optional<std::pair<std::string, uint16_t>> parseHostPort(const std::string& text)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size())
        return std::nullopt;
    unsigned port = 0;
    const char* first = text.data() + colon + 1;
    const char* last = text.data() + text.size();
    const auto [end, ec] = std::from_chars(first, last, port);
    if (ec != std::errc() || end != last || port == 0 || port > 65535)
        return std::nullopt;
    return std::pair(text.substr(0, colon), static_cast<uint16_t>(port));
}

WorldClient::WorldClient(Network& network, AuthClient& auth) : network_(network), auth_(auth) {}

void WorldClient::reset(std::optional<AuthConfig> config)
{
    if (!conn_.empty())
        network_.close(conn_);
    enabled_ = config.has_value();
    caPem_ = config ? config->caPem : std::string();
    state_ = WorldState::Disconnected;
    conn_.clear();
    inbox_.clear();
    realms_ = nullptr;
    pendingRealm_.reset();
    realmsRequest_ = 0;
    events_.clear();
}

void WorldClient::setAddressOverride(std::optional<std::string> hostPort)
{
    if (hostPort && !parseHostPort(*hostPort))
        throw std::invalid_argument(std::format("world address override '{}' is not host:port", *hostPort));
    override_ = std::move(hostPort);
}

void WorldClient::require(WorldState state, const char* function) const
{
    if (!enabled_)
        throw std::logic_error(std::format("{}: Online Services are disabled", function));
    if (state_ != state)
        throw std::logic_error(std::format("{}: needs state '{}', the connection is '{}'", function,
                                           worldStateName(state), worldStateName(state_)));
}

void WorldClient::connect(uint64_t realmId)
{
    require(WorldState::Disconnected, "connect");
    if (!auth_.loggedIn())
        return emit("world.disconnected", {{"code", "not_logged_in"}, {"reason", "log in with kast.auth first"}});
    state_ = WorldState::Connecting;
    pendingRealm_ = realmId;
    if (const Json* realm = findRealm(realms_, realmId))
        return open(*realm);
    realmsRequest_ = auth_.realms(); // answered through observe()
}

void WorldClient::observe(const NetEvent& authEvent)
{
    const uint64_t id = authEvent.payload.value("id", uint64_t{0});
    if (authEvent.name == "auth.realms")
        realms_ = authEvent.payload.value("realms", Json());
    if (!pendingRealm_ || !realmsRequest_ || id != realmsRequest_)
        return;
    realmsRequest_ = 0;
    if (authEvent.name == "auth.error")
        return drop("realm_lookup_failed", std::format("realm list: {} ({})", authEvent.payload.value("code", ""),
                                                       authEvent.payload.value("message", "")));
    if (const Json* realm = findRealm(realms_, *pendingRealm_))
        return open(*realm);
    drop("unknown_realm", std::format("no realm with id {}", *pendingRealm_));
}

void WorldClient::open(const Json& realm)
{
    pendingRealm_.reset();
    std::string host = realm.value("host", "");
    uint16_t port = realm.value("port", uint16_t{0});
    if (override_)
        std::tie(host, port) = *parseHostPort(*override_);
    const std::optional<std::string> token = auth_.accessToken();
    if (!token)
        return drop("not_logged_in", "logged out before the connection started");
    conn_ = std::format("{}{}", kConnPrefix, ++serial_);
    NetOpenOptions options;
    options.name = conn_;
    options.type = NetType::Tcp;
    options.host = host;
    options.port = port;
    options.tls = true;
    options.caPem = caPem_;
    options.encoding = NetEncoding::Base64; // binary frames; split by proto::decode_frame, not by Network framing
    options.framing = NetFraming::Raw;      // the world's u16 length doesn't count the opcode: U16le can't split it
    try {
        network_.open(std::move(options));
    } catch (const std::invalid_argument& e) {
        return drop("connect_failed", e.what());
    }
    log::info(std::format("World: connecting to realm {} at {}:{}{}", realm.value("id", uint64_t{0}), host, port,
                          override_ ? " (address override)" : ""));
    send(proto::Hello{});
    send(proto::Auth{*token}); // queued until TLS is up
}

void WorldClient::disconnect()
{
    if (state_ != WorldState::Disconnected)
        drop("client_disconnect", "disconnected by script");
}

void WorldClient::listCharacters()
{
    require(WorldState::Lobby, "listCharacters");
    send(proto::CharListReq{});
}

void WorldClient::createCharacter(const std::string& name, const std::map<std::string, std::string>& options)
{
    require(WorldState::Lobby, "createCharacter");
    send(proto::CharCreate{name, options});
}

void WorldClient::deleteCharacter(uint64_t charId)
{
    require(WorldState::Lobby, "deleteCharacter");
    send(proto::CharDelete{charId});
}

void WorldClient::enterWorld(uint64_t charId)
{
    require(WorldState::Lobby, "enterWorld");
    send(proto::EnterWorld{charId});
}

void WorldClient::say(const std::string& text)
{
    require(WorldState::InWorld, "say");
    send(proto::Say{text});
}

void WorldClient::sendEvent(const std::string& name, const nlohmann::json& data)
{
    if (!proto::valid_client_event_name(name))
        throw std::logic_error(std::format("send: name must be 1-{} characters of a-z, 0-9, '.' and '_', got '{}'",
                                           proto::kClientEventNameMax, name));
    std::string json = data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (json.size() > proto::kClientEventJsonMax)
        throw std::logic_error(std::format("send: data is {} bytes encoded, the limit is {}", json.size(),
                                           proto::kClientEventJsonMax));
    require(WorldState::InWorld, "send");
    send(proto::ClientEvent{name, std::move(json)});
}

void WorldClient::interact(uint32_t objectId)
{
    require(WorldState::InWorld, "interact");
    send(proto::Interact{objectId});
}

void WorldClient::move(float x, float y, float z, float heading)
{
    require(WorldState::InWorld, "move");
    // clientTimeMs is informational (the server trusts its own clock): wraps after 49 days, which is fine.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    send(proto::Move{{x, y, z}, heading,
                     static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count())});
}

void WorldClient::send(const auto& message)
{
    const std::vector<uint8_t> frame = proto::encode(proto::Message{message});
    network_.send(conn_, std::string(frame.begin(), frame.end()));
}

bool WorldClient::handle(const NetEvent& event)
{
    const std::string conn = event.payload.value("conn", "");
    if (!conn.starts_with(kConnPrefix))
        return false;
    if (conn != conn_)
        return true; // an old connection's late result
    if (event.name == "net.message") {
        std::optional<std::string> bytes = base64Decode(event.payload.value("data", ""));
        if (bytes)
            received(*bytes);
    } else if (event.name == "net.closed") {
        // A world.disconnected with the server's reason went first if it sent Disconnect (then conn_ is cleared).
        drop(state_ == WorldState::Connecting ? "connect_failed" : "connection_closed",
             event.payload.value("reason", "closed"));
    } // net.open: Hello and Auth are already queued; net.error: its message is net.closed's reason too
    return true;
}

void WorldClient::received(const std::string& bytes)
{
    inbox_ += bytes;
    size_t at = 0;
    while (!conn_.empty()) {
        const auto* data = reinterpret_cast<const uint8_t*>(inbox_.data()) + at;
        const proto::Decoded frame = proto::decode_frame({data, inbox_.size() - at}, kMaxPayload);
        if (frame.status == proto::DecodeStatus::need_more)
            break;
        if (frame.status != proto::DecodeStatus::ok) {
            const proto::Header header = proto::parse_header({data, proto::kHeaderSize});
            const bool unknown = frame.status == proto::DecodeStatus::unknown_opcode;
            return drop(unknown ? "unknown_opcode" : "malformed",
                        unknown ? std::format("the server sent unknown opcode 0x{:04x}", static_cast<int>(header.opcode))
                                : std::format("the server sent a malformed {} frame", proto::opcode_name(header.opcode)));
        }
        at += frame.consumed;
        std::visit([this](const auto& m) { dispatch(m); }, frame.msg);
    }
    if (!conn_.empty())
        inbox_.erase(0, at);
}

void WorldClient::dispatch(const auto& m)
{
    using T = std::decay_t<decltype(m)>;
    constexpr bool beforeAuth = std::is_same_v<T, proto::AuthOk> || std::is_same_v<T, proto::Disconnect> ||
                                std::is_same_v<T, proto::Ping> || std::is_same_v<T, proto::Pong>;
    if (state_ == WorldState::Connecting && !beforeAuth)
        return drop("unexpected_message", std::format("the server sent {} before AuthOk", proto::opcode_name(T::op)));
    if constexpr (std::is_same_v<T, proto::AuthOk>) {
        if (state_ != WorldState::Connecting)
            return drop("unexpected_message", "the server sent AuthOk twice");
        state_ = WorldState::Lobby;
        log::info(std::format("World: connected as account {}", m.accountId));
        emit("world.connected", {{"accountId", m.accountId}, {"role", roleName(m.role)}});
    } else if constexpr (std::is_same_v<T, proto::Disconnect>) {
        drop(proto::code_name(m.reason),
             std::format("disconnected by the server (code {})", static_cast<int>(m.reason)));
    } else if constexpr (std::is_same_v<T, proto::Ping>) {
        send(proto::Pong{m.nonce});
    } else if constexpr (std::is_same_v<T, proto::Pong>) {
        // the client never pings
    } else if constexpr (std::is_same_v<T, proto::CharList>) {
        Json list = Json::array();
        for (const proto::CharInfo& c : m.chars)
            list.push_back(character(c));
        emit("world.charList", {{"characters", std::move(list)}});
    } else if constexpr (std::is_same_v<T, proto::CharCreated>) {
        emit("world.charCreated", {{"character", character(m.character)}});
    } else if constexpr (std::is_same_v<T, proto::CharDeleted>) {
        emit("world.charDeleted", {{"charId", m.charId}});
    } else if constexpr (std::is_same_v<T, proto::CharError>) {
        emit("world.charError", {{"request", proto::opcode_name(m.request)}, {"code", proto::code_name(m.code)}});
    } else if constexpr (std::is_same_v<T, proto::EnterWorldOk>) {
        state_ = WorldState::InWorld;
        emit("world.entered",
             {{"entityId", m.entityId}, {"map", m.map}, {"position", position(m.pos)}, {"heading", m.heading}});
    } else if constexpr (std::is_same_v<T, proto::MapChange>) {
        emit("world.mapChanged", {{"map", m.map}, {"position", position(m.pos)}, {"heading", m.heading}});
    } else if constexpr (std::is_same_v<T, proto::Spawn>) {
        emit("world.spawn", {{"entityId", m.entityId},
                             {"name", m.name},
                             {"position", position(m.pos)},
                             {"heading", m.heading},
                             {"kind", m.kind == proto::SpawnKind::npc ? "npc" : "player"},
                             {"template", m.templateName}});
    } else if constexpr (std::is_same_v<T, proto::Despawn>) {
        emit("world.despawn", {{"entityId", m.entityId}});
    } else if constexpr (std::is_same_v<T, proto::EntityMove>) {
        emit("world.entityMove", {{"entityId", m.entityId}, {"position", position(m.pos)}, {"heading", m.heading}});
    } else if constexpr (std::is_same_v<T, proto::Correction>) {
        emit("world.correction", {{"position", position(m.pos)}});
    } else if constexpr (std::is_same_v<T, proto::ChatMsg>) {
        emit("world.chat", {{"from", m.fromName}, {"text", m.text}}); // never logged (world server rule)
    } else if constexpr (std::is_same_v<T, proto::ComponentUpdate>) {
        Json data = Json::parse(m.json, nullptr, false);
        if (data.is_discarded()) // the server only sends valid JSON; one bad value isn't worth the connection
            return log::warn(std::format("World: ignored ComponentUpdate \"{}\" with invalid JSON", m.name));
        emit("world.component", {{"entityId", m.entityId}, {"name", m.name}, {"data", std::move(data)}});
    } else if constexpr (std::is_same_v<T, proto::ComponentRemove>) {
        emit("world.componentRemoved", {{"entityId", m.entityId}, {"name", m.name}});
    } else if constexpr (std::is_same_v<T, proto::ServerEvent>) {
        Json payload = Json::parse(m.json, nullptr, false);
        if (payload.is_discarded())
            return log::warn(std::format("World: ignored ServerEvent \"{}\" with invalid JSON", m.name));
        emit("server." + m.name, std::move(payload));
    } else { // a client-to-server message
        drop("unexpected_message", std::format("the server sent client message {}", proto::opcode_name(T::op)));
    }
}

void WorldClient::drop(const std::string& code, const std::string& reason)
{
    if (!conn_.empty())
        network_.close(conn_);
    conn_.clear();
    inbox_.clear();
    pendingRealm_.reset();
    realmsRequest_ = 0;
    state_ = WorldState::Disconnected;
    log::info(std::format("World: disconnected ({}): {}", code, reason));
    emit("world.disconnected", {{"code", code}, {"reason", reason}});
}

void WorldClient::emit(std::string name, Json payload) { events_.push_back({std::move(name), std::move(payload)}); }

std::vector<NetEvent> WorldClient::poll()
{
    std::vector<NetEvent> out;
    out.swap(events_);
    return out;
}

} // namespace kast
