// Every wire message: opcode, struct and payload layout (see PROTOCOL.md). C = client->server, S = server->client.
#pragma once
#include "wire.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace kast::proto {

constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 4; // u16 payload length + u16 opcode
constexpr size_t kMaxPayloadBytes = 0xFFFF; // the u16 length field's limit

enum class Opcode : uint16_t {
    Hello = 0x0001,        // C
    Auth = 0x0002,         // C
    Ping = 0x0003,         // C/S
    Pong = 0x0004,         // C/S
    CharListReq = 0x0010,  // C
    CharCreate = 0x0011,   // C
    CharDelete = 0x0012,   // C
    EnterWorld = 0x0013,   // C
    Move = 0x0020,         // C
    Say = 0x0021,          // C
    AuthOk = 0x0102,       // S
    Disconnect = 0x0103,   // S
    CharList = 0x0110,     // S
    CharCreated = 0x0111,  // S
    CharDeleted = 0x0112,  // S
    EnterWorldOk = 0x0113, // S
    CharError = 0x0114,    // S
    Spawn = 0x0120,        // S
    Despawn = 0x0121,      // S
    EntityMove = 0x0122,   // S
    Correction = 0x0123,   // S
    ChatMsg = 0x0124,      // S
    ComponentUpdate = 0x0125, // S
    ComponentRemove = 0x0126, // S
    ServerEvent = 0x0127,     // S
};

// Reason codes carried by Disconnect and CharError.
enum class Code : uint16_t {
    ok = 0,
    protocol_mismatch = 1,
    malformed = 2,
    unknown_opcode = 3,
    frame_too_large = 4,
    rate_limited = 5,
    handshake_timeout = 6,
    idle_timeout = 7,
    invalid_token = 8,
    token_expired = 9,
    service_unavailable = 10,
    logged_in_elsewhere = 11,
    server_shutdown = 12,
    server_full = 13,
    unexpected_message = 14,
    name_taken = 20,
    name_invalid = 21,
    char_limit = 22,
    not_found = 23,
    already_in_world = 24,
};

const char* code_name(Code c);

enum class Role : uint8_t { player = 0, gm = 1, admin = 2 };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    bool operator==(const Vec3&) const = default;
};
inline void write(Writer& w, const Vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
inline void read(Reader& r, Vec3& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); }

struct Hello {
    static constexpr Opcode op = Opcode::Hello;
    uint16_t protocolVersion = kProtocolVersion;
    void write(Writer& w) const { w.u16(protocolVersion); }
    void read(Reader& r) { protocolVersion = r.u16(); }
    bool operator==(const Hello&) const = default;
};

struct Auth {
    static constexpr Opcode op = Opcode::Auth;
    std::string accessToken;
    void write(Writer& w) const { w.str(accessToken); }
    void read(Reader& r) { accessToken = r.str(); }
    bool operator==(const Auth&) const = default;
};

struct Ping {
    static constexpr Opcode op = Opcode::Ping;
    uint32_t nonce = 0;
    void write(Writer& w) const { w.u32(nonce); }
    void read(Reader& r) { nonce = r.u32(); }
    bool operator==(const Ping&) const = default;
};

struct Pong {
    static constexpr Opcode op = Opcode::Pong;
    uint32_t nonce = 0;
    void write(Writer& w) const { w.u32(nonce); }
    void read(Reader& r) { nonce = r.u32(); }
    bool operator==(const Pong&) const = default;
};

struct CharListReq {
    static constexpr Opcode op = Opcode::CharListReq;
    void write(Writer&) const {}
    void read(Reader&) {}
    bool operator==(const CharListReq&) const = default;
};

struct CharCreate {
    static constexpr Opcode op = Opcode::CharCreate;
    std::string name;
    void write(Writer& w) const { w.str(name); }
    void read(Reader& r) { name = r.str(); }
    bool operator==(const CharCreate&) const = default;
};

struct CharDelete {
    static constexpr Opcode op = Opcode::CharDelete;
    uint64_t charId = 0;
    void write(Writer& w) const { w.u64(charId); }
    void read(Reader& r) { charId = r.u64(); }
    bool operator==(const CharDelete&) const = default;
};

struct EnterWorld {
    static constexpr Opcode op = Opcode::EnterWorld;
    uint64_t charId = 0;
    void write(Writer& w) const { w.u64(charId); }
    void read(Reader& r) { charId = r.u64(); }
    bool operator==(const EnterWorld&) const = default;
};

struct Move {
    static constexpr Opcode op = Opcode::Move;
    Vec3 pos;
    float heading = 0;
    uint32_t clientTimeMs = 0;
    void write(Writer& w) const { proto::write(w, pos); w.f32(heading); w.u32(clientTimeMs); }
    void read(Reader& r) { proto::read(r, pos); heading = r.f32(); clientTimeMs = r.u32(); }
    bool operator==(const Move&) const = default;
};

struct Say {
    static constexpr Opcode op = Opcode::Say;
    std::string text;
    void write(Writer& w) const { w.str(text); }
    void read(Reader& r) { text = r.str(); }
    bool operator==(const Say&) const = default;
};

struct AuthOk {
    static constexpr Opcode op = Opcode::AuthOk;
    uint64_t accountId = 0;
    Role role = Role::player;
    void write(Writer& w) const { w.u64(accountId); w.u8(static_cast<uint8_t>(role)); }
    void read(Reader& r) {
        accountId = r.u64();
        const uint8_t v = r.u8();
        if (v > static_cast<uint8_t>(Role::admin)) r.fail();
        role = static_cast<Role>(v);
    }
    bool operator==(const AuthOk&) const = default;
};

struct Disconnect {
    static constexpr Opcode op = Opcode::Disconnect;
    Code reason = Code::ok;
    void write(Writer& w) const { w.u16(static_cast<uint16_t>(reason)); }
    void read(Reader& r) { reason = static_cast<Code>(r.u16()); }
    bool operator==(const Disconnect&) const = default;
};

struct CharInfo {
    uint64_t id = 0;
    std::string name;
    Vec3 pos;
    bool operator==(const CharInfo&) const = default;
};
inline void write(Writer& w, const CharInfo& c) { w.u64(c.id); w.str(c.name); write(w, c.pos); }
inline void read(Reader& r, CharInfo& c) { c.id = r.u64(); c.name = r.str(); read(r, c.pos); }

struct CharList {
    static constexpr Opcode op = Opcode::CharList;
    std::vector<CharInfo> chars; // at most 255 on the wire
    void write(Writer& w) const {
        const size_t n = chars.size() > 255 ? 255 : chars.size();
        w.u8(static_cast<uint8_t>(n));
        for (size_t i = 0; i < n; ++i) proto::write(w, chars[i]);
    }
    void read(Reader& r) {
        chars.resize(r.u8());
        for (auto& c : chars) proto::read(r, c);
    }
    bool operator==(const CharList&) const = default;
};

struct CharCreated {
    static constexpr Opcode op = Opcode::CharCreated;
    CharInfo character;
    void write(Writer& w) const { proto::write(w, character); }
    void read(Reader& r) { proto::read(r, character); }
    bool operator==(const CharCreated&) const = default;
};

struct CharDeleted {
    static constexpr Opcode op = Opcode::CharDeleted;
    uint64_t charId = 0;
    void write(Writer& w) const { w.u64(charId); }
    void read(Reader& r) { charId = r.u64(); }
    bool operator==(const CharDeleted&) const = default;
};

struct EnterWorldOk {
    static constexpr Opcode op = Opcode::EnterWorldOk;
    uint32_t entityId = 0;
    Vec3 pos;
    float heading = 0;
    void write(Writer& w) const { w.u32(entityId); proto::write(w, pos); w.f32(heading); }
    void read(Reader& r) { entityId = r.u32(); proto::read(r, pos); heading = r.f32(); }
    bool operator==(const EnterWorldOk&) const = default;
};

// Non-fatal error for a character request; the connection stays open.
struct CharError {
    static constexpr Opcode op = Opcode::CharError;
    Opcode request = Opcode::CharListReq;
    Code code = Code::ok;
    void write(Writer& w) const { w.u16(static_cast<uint16_t>(request)); w.u16(static_cast<uint16_t>(code)); }
    void read(Reader& r) { request = static_cast<Opcode>(r.u16()); code = static_cast<Code>(r.u16()); }
    bool operator==(const CharError&) const = default;
};

struct Spawn {
    static constexpr Opcode op = Opcode::Spawn;
    uint32_t entityId = 0;
    std::string name;
    Vec3 pos;
    float heading = 0;
    void write(Writer& w) const { w.u32(entityId); w.str(name); proto::write(w, pos); w.f32(heading); }
    void read(Reader& r) { entityId = r.u32(); name = r.str(); proto::read(r, pos); heading = r.f32(); }
    bool operator==(const Spawn&) const = default;
};

struct Despawn {
    static constexpr Opcode op = Opcode::Despawn;
    uint32_t entityId = 0;
    void write(Writer& w) const { w.u32(entityId); }
    void read(Reader& r) { entityId = r.u32(); }
    bool operator==(const Despawn&) const = default;
};

struct EntityMove {
    static constexpr Opcode op = Opcode::EntityMove;
    uint32_t entityId = 0;
    Vec3 pos;
    float heading = 0;
    void write(Writer& w) const { w.u32(entityId); proto::write(w, pos); w.f32(heading); }
    void read(Reader& r) { entityId = r.u32(); proto::read(r, pos); heading = r.f32(); }
    bool operator==(const EntityMove&) const = default;
};

struct Correction {
    static constexpr Opcode op = Opcode::Correction;
    Vec3 pos;
    void write(Writer& w) const { proto::write(w, pos); }
    void read(Reader& r) { proto::read(r, pos); }
    bool operator==(const Correction&) const = default;
};

struct ChatMsg {
    static constexpr Opcode op = Opcode::ChatMsg;
    std::string fromName;
    std::string text;
    void write(Writer& w) const { w.str(fromName); w.str(text); }
    void read(Reader& r) { fromName = r.str(); text = r.str(); }
    bool operator==(const ChatMsg&) const = default;
};

// A Lua component's full value as JSON (README "Lua components and systems"). Server-authoritative: never C->S.
struct ComponentUpdate {
    static constexpr Opcode op = Opcode::ComponentUpdate;
    uint32_t entityId = 0;
    std::string name;
    std::string json;
    void write(Writer& w) const { w.u32(entityId); w.str(name); w.str(json); }
    void read(Reader& r) { entityId = r.u32(); name = r.str(); json = r.str(); }
    bool operator==(const ComponentUpdate&) const = default;
    // Whether a component of these sizes fits one frame (str would truncate it otherwise).
    static bool fits(size_t nameBytes, size_t jsonBytes) { return 4 + 2 + nameBytes + 2 + jsonBytes <= kMaxPayloadBytes; }
};

struct ComponentRemove {
    static constexpr Opcode op = Opcode::ComponentRemove;
    uint32_t entityId = 0;
    std::string name;
    void write(Writer& w) const { w.u32(entityId); w.str(name); }
    void read(Reader& r) { entityId = r.u32(); name = r.str(); }
    bool operator==(const ComponentRemove&) const = default;
};

// A named event a Lua system sent with kast.sendTo/sendNearby/broadcast (PROTOCOL.md "Server events"). Never C->S.
struct ServerEvent {
    static constexpr Opcode op = Opcode::ServerEvent;
    std::string name;
    std::string json;
    void write(Writer& w) const { w.str(name); w.str(json); }
    void read(Reader& r) { name = r.str(); json = r.str(); }
    bool operator==(const ServerEvent&) const = default;
    // Whether an event of these sizes fits one frame (str would truncate it otherwise).
    static bool fits(size_t nameBytes, size_t jsonBytes) { return 2 + nameBytes + 2 + jsonBytes <= kMaxPayloadBytes; }
};

using Message = std::variant<Hello, Auth, Ping, Pong, CharListReq, CharCreate, CharDelete, EnterWorld, Move, Say,
                             AuthOk, Disconnect, CharList, CharCreated, CharDeleted, EnterWorldOk, CharError, Spawn,
                             Despawn, EntityMove, Correction, ChatMsg, ComponentUpdate,
                             ComponentRemove, ServerEvent>;

} // namespace kast::proto
