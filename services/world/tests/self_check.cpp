// Assert-style checks for the world server's pure logic: frame codec (every message), frame rejection, names,
// movement, AOI sets, chat sanitizing and rate limits, config validation, tick stats and JWT/JWKS verification with
// locally generated RSA keys; lua_checks.cpp covers the Lua layer and the maps (manifests, World on several maps). No network, database or sleeping: time is passed in.
// Exit 0 if every check passes. Run by CTest as world_self_check.
#include "auth/jwks_cache.h"
#include "auth/jwt_verify.h"
#include "core/config.h"
#include "core/rate_limiter.h"
#include "core/tick_stats.h"
#include "net/session.h"
#include "world/aoi_grid.h"
#include "world/rules.h"

#include <codec.h>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cstdio>
#include <map>
#include <source_location>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace kast;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, std::source_location where = std::source_location::current()) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAILED line %u: %s\n", static_cast<unsigned>(where.line()), what);
}

const TimePoint T0{std::chrono::seconds(1000)};

// --- protocol ---

template <typename T>
void roundtrip(const T& msg, const char* name) {
    const auto frame = proto::encode(proto::Message{msg});
    const auto d = proto::decode_frame(frame, 4096);
    const bool ok = d.status == proto::DecodeStatus::ok && d.consumed == frame.size() &&
                    std::holds_alternative<T>(d.msg) && std::get<T>(d.msg) == msg;
    check(ok, name);
}

void checkCodecRoundTrip() {
    using namespace proto;
    roundtrip(Hello{7}, "Hello");
    roundtrip(Auth{"header.payload.sig"}, "Auth");
    roundtrip(Ping{0xDEADBEEF}, "Ping");
    roundtrip(Pong{42}, "Pong");
    roundtrip(CharListReq{}, "CharListReq");
    roundtrip(CharCreate{"Aragorn"}, "CharCreate");
    roundtrip(CharCreate{"Gandalf", {{"class", "mage"}, {"hair", "grey beard"}}}, "CharCreate with options");
    roundtrip(CharDelete{1234567890123ull}, "CharDelete");
    roundtrip(EnterWorld{99}, "EnterWorld");
    roundtrip(Move{{1.5f, -2.25f, 1000.f}, 3.14f, 123456}, "Move");
    roundtrip(Say{"hello \xC3\xA9"}, "Say");
    roundtrip(AuthOk{77, Role::gm}, "AuthOk");
    roundtrip(Disconnect{Code::logged_in_elsewhere}, "Disconnect");
    roundtrip(CharList{{{1, "Alice", {1, 2, 3}}, {2, "Bob", {4, 5, 6}}}}, "CharList");
    roundtrip(CharList{}, "CharList empty");
    roundtrip(CharCreated{{5, "Carol", {7, 8, 9}}}, "CharCreated");
    roundtrip(CharCreated{{6, "Dora", {1, 2, 3}, {{"class", "rogue"}}}}, "CharCreated with options");
    roundtrip(CharList{{{1, "Alice", {1, 2, 3}, {{"class", "priest"}}}, {2, "Bob", {4, 5, 6}}}}, "CharList with options");
    roundtrip(CharDeleted{5}, "CharDeleted");
    roundtrip(EnterWorldOk{11, 2, {1, 0, 1}, 1.f}, "EnterWorldOk");
    roundtrip(MapChange{65535, {-3, 4, 5000}, 2.5f, 0xFFFFFFFE}, "MapChange");
    roundtrip(CharError{Opcode::CharCreate, Code::name_taken}, "CharError");
    roundtrip(Spawn{3, "Dave", {10, 0, 10}, 0.5f, SpawnKind::player, ""}, "Spawn");
    roundtrip(Spawn{4, "Guard", {12, 1, 10}, 1.5f, SpawnKind::npc, "Guard"}, "Spawn npc");
    roundtrip(Despawn{3}, "Despawn");
    roundtrip(EntityMove{3, {11, 0, 12}, 0.25f}, "EntityMove");
    roundtrip(Correction{{1, 2, 3}, 0xFFFFFFFEu, 7}, "Correction");
    roundtrip(Move{{1, 2, 3}, 0.5f, 99, 0xFFFFFFFFu, 3}, "Move");
    roundtrip(ChatMsg{"Eve", "hi all"}, "ChatMsg");

    // exact byte layout: u16 len, u16 opcode, payload, little-endian
    const auto f = encode(Message{Hello{0x0102}});
    check(f == std::vector<uint8_t>{2, 0, 1, 0, 0x02, 0x01}, "Hello bytes little-endian");

    roundtrip(ClientEvent{"combat.power", R"({"power":"strike"})", 0xFFFFFFFEu}, "ClientEvent with requestId");
    roundtrip(Reply{0xFFFFFFFEu, true, R"({"power":"strike"})"}, "Reply ok");
    roundtrip(Reply{3, false, R"("range")"}, "Reply rejected");
    check(kProtocolVersion == 7, "protocol version 7");
    // v7 ClientEvent: str name, str json, u32 requestId (0: kast.world.send); Reply: u32 requestId, u8 ok (0/1), str json
    const auto cev = encode(Message{ClientEvent{"a", "{}", 0x01020304}});
    check(cev.size() == 4 + 3 + 4 + 4 && cev[11] == 0x04 && cev[14] == 0x01, "ClientEvent requestId after json");
    const std::vector<uint8_t> badOk{1, 0, 0, 0, 2, 0, 0};
    Message replyOut;
    check(decode_payload(Opcode::Reply, badOk, replyOut) == DecodeStatus::malformed, "a Reply with ok 2 is malformed");
    // v6 Move: vec3 pos, f32 heading, u32 clientTimeMs, u32 seq, u32 epoch; Correction: vec3 pos, u32 ackSeq, u32 epoch
    const auto mv = encode(Message{Move{{0, 0, 0}, 0, 0, 0x01020304, 0x05060708}});
    check(mv.size() == 4 + 12 + 4 + 4 + 4 + 4 && mv[24] == 0x04 && mv[27] == 0x01 && mv[28] == 0x08 && mv[31] == 0x05,
          "Move seq then epoch after clientTimeMs");
    const auto co = encode(Message{Correction{{0, 0, 0}, 0x01020304, 0x05060708}});
    check(co.size() == 4 + 12 + 4 + 4 && co[16] == 0x04 && co[20] == 0x08, "Correction ackSeq then epoch after pos");
    std::vector<uint8_t> v5Move(12 + 4 + 4); // a v5 Move: no seq, no epoch
    Message v5Out;
    check(decode_payload(Opcode::Move, v5Move, v5Out) == DecodeStatus::malformed, "a v5 Move is malformed in v6");
    // v4 CharCreate: str name, u8 count, count x (str key, str value), keys in order; a key twice is malformed
    const auto cc = encode(Message{CharCreate{"Al", {{"b", "2"}, {"a", "1"}}}});
    check(cc == std::vector<uint8_t>{17, 0, 0x11, 0, 2, 0, 'A', 'l', 2, 1, 0, 'a', 1, 0, '1', 1, 0, 'b', 1, 0, '2'},
          "CharCreate options bytes");
    const std::vector<uint8_t> dupe{2, 0, 'A', 'l', 2, 1, 0, 'a', 1, 0, '1', 1, 0, 'a', 1, 0, '2'};
    Message dupeOut;
    check(decode_payload(Opcode::CharCreate, dupe, dupeOut) == DecodeStatus::malformed, "CharCreate with a key twice is malformed");
    const std::vector<uint8_t> shortOpts{2, 0, 'A', 'l', 1, 1, 0, 'a'};
    check(decode_payload(Opcode::CharCreate, shortOpts, dupeOut) == DecodeStatus::malformed, "CharCreate with a cut option is malformed");
    // C++ limits on options (valid_char_options): <= 8 entries, keys 1-16 of [a-z_], values <= 32 printable ASCII
    check(valid_char_options({}) && valid_char_options({{"class", "warrior"}}) &&
              valid_char_options({{"a_b", std::string(32, '~')}, {std::string(16, 'z'), ""}}),
          "options: empty, plain, and at the limits are valid");
    CharOptions nine;
    for (char k = 'a'; k <= 'i'; ++k) nine[std::string(1, k)] = "x";
    check(!valid_char_options(nine), "options: 9 entries refused");
    check(!valid_char_options({{"", "x"}}) && !valid_char_options({{std::string(17, 'a'), "x"}}) &&
              !valid_char_options({{"Class", "x"}}) && !valid_char_options({{"class1", "x"}}) &&
              !valid_char_options({{"a.b", "x"}}),
          "options: empty, long, upper-case, digit and dot keys refused");
    check(!valid_char_options({{"a", std::string(33, 'x')}}) && !valid_char_options({{"a", "tab\t"}}) &&
              !valid_char_options({{"a", "\xC3\xA9"}}),
          "options: long, control and non-ASCII values refused");
    // v3 EnterWorldOk: u32 entityId, u16 map, vec3 pos, f32 heading; v6 MapChange: u16 map, vec3 pos, f32 heading, u32 epoch
    const auto ewo = encode(Message{EnterWorldOk{1, 0x0201, {0, 0, 0}, 0}});
    check(ewo.size() == 4 + 4 + 2 + 12 + 4 && ewo[8] == 0x01 && ewo[9] == 0x02, "EnterWorldOk map follows entityId");
    const auto mc = encode(Message{MapChange{0x0201, {0, 0, 0}, 0}});
    check(mc.size() == 4 + 2 + 12 + 4 + 4 && mc[2] == 0x28 && mc[3] == 0x01 && mc[4] == 0x01 && mc[5] == 0x02,
          "MapChange opcode 0x0128, map first");
    check(std::string(opcode_name(Opcode::MapChange)) == "MapChange", "MapChange name");
    // ClientEvent: opcode 0x0022, str name, str json (ServerEvent's layout), v7 u32 requestId
    const auto ce = encode(Message{ClientEvent{"a.b", "{}"}});
    check(ce == std::vector<uint8_t>{13, 0, 0x22, 0, 3, 0, 'a', '.', 'b', 2, 0, '{', '}', 0, 0, 0, 0}, "ClientEvent bytes");
    check(std::string(opcode_name(Opcode::ClientEvent)) == "ClientEvent", "ClientEvent name");
    // v5 Interact: opcode 0x0023, u32 objectId
    roundtrip(Interact{0xFFFFFFFFu}, "Interact");
    check(encode(Message{Interact{0x01020304}}) == std::vector<uint8_t>{4, 0, 0x23, 0, 4, 3, 2, 1}, "Interact bytes");
    check(std::string(opcode_name(Opcode::Interact)) == "Interact", "Interact name");
    // v2 Spawn: u32 id, str name, vec3, f32 heading, u8 kind, str template
    const auto npc = encode(Message{Spawn{1, "G", {0, 0, 0}, 0, SpawnKind::npc, "T"}});
    check(npc.size() == 4 + 4 + 3 + 12 + 4 + 1 + 3 && npc[4 + 4 + 3 + 16] == 1 && npc.back() == 'T',
          "Spawn kind and template follow the heading");
    auto badKind = npc;
    badKind[4 + 4 + 3 + 16] = 2;
    check(decode_frame(badKind, 4096).status == DecodeStatus::malformed, "Spawn kind 2 is malformed");
    auto v1 = encode(Message{Spawn{1, "G", {0, 0, 0}, 0, SpawnKind::player, ""}});
    v1.resize(v1.size() - 3); // a v1 Spawn: no kind, no template
    v1[0] = static_cast<uint8_t>(v1.size() - 4);
    check(decode_frame(v1, 4096).status == DecodeStatus::malformed, "a v1 Spawn is malformed in v2");

    // two frames back to back decode one at a time
    auto two = encode(Message{Ping{1}});
    encode(two, Message{Pong{2}});
    const auto d1 = decode_frame(two, 4096);
    check(d1.status == DecodeStatus::ok && std::get<Ping>(d1.msg).nonce == 1, "first of two frames");
    const auto d2 = decode_frame(std::span(two).subspan(d1.consumed), 4096);
    check(d2.status == DecodeStatus::ok && std::get<Pong>(d2.msg).nonce == 2, "second of two frames");
}

void checkFrameRejection() {
    using namespace proto;
    // oversized: header alone is enough to reject, before the payload arrives
    std::vector<uint8_t> big = {0x01, 0x10, 0x21, 0x00}; // len 4097, Say
    check(decode_frame(big, 4096).status == DecodeStatus::too_large, "oversized frame rejected from header");
    // truncated: header promises more than arrived
    auto f = encode(Message{Say{"hello"}});
    f.pop_back();
    check(decode_frame(f, 4096).status == DecodeStatus::need_more, "partial frame waits for more");
    check(decode_frame(std::span(f).first(3), 4096).status == DecodeStatus::need_more, "partial header waits");
    // truncated payload inside a complete frame: string length longer than payload
    std::vector<uint8_t> lie = {4, 0, 0x21, 0x00, 10, 0, 'a', 'b'}; // Say claims 10 chars, has 2
    check(decode_frame(lie, 4096).status == DecodeStatus::malformed, "string past payload end is malformed");
    // trailing bytes
    std::vector<uint8_t> extra = {3, 0, 0x01, 0x00, 1, 0, 9};
    check(decode_frame(extra, 4096).status == DecodeStatus::malformed, "trailing bytes are malformed");
    // short fixed field
    std::vector<uint8_t> shortHello = {1, 0, 0x01, 0x00, 1};
    check(decode_frame(shortHello, 4096).status == DecodeStatus::malformed, "short Hello is malformed");
    // unknown opcode
    std::vector<uint8_t> unk = {0, 0, 0xFF, 0x7F};
    check(decode_frame(unk, 4096).status == DecodeStatus::unknown_opcode, "unknown opcode");
    // NaN coordinate
    auto mv = encode(Message{Move{{1, 2, 3}, 0, 0}});
    mv[4] = 0x00; mv[5] = 0x00; mv[6] = 0xC0; mv[7] = 0x7F; // x = NaN
    check(decode_frame(mv, 4096).status == DecodeStatus::malformed, "NaN coordinate is malformed");
    // bad enum
    std::vector<uint8_t> role = {9, 0, 0x02, 0x01, 1, 0, 0, 0, 0, 0, 0, 0, 9};
    check(decode_frame(role, 4096).status == DecodeStatus::malformed, "unknown role is malformed");
}

// --- rules ---

void checkNames() {
    check(valid_character_name("Bob"), "3 letters ok");
    check(valid_character_name("AbcdefghijklmnoP"), "16 letters ok");
    check(!valid_character_name("Al"), "2 letters rejected");
    check(!valid_character_name("Abcdefghijklmnopq"), "17 letters rejected");
    check(!valid_character_name("Bob1"), "digit rejected");
    check(!valid_character_name("Bo b"), "space rejected");
    check(!valid_character_name("B\xC3\xA9o"), "non-ASCII rejected");
    check(!valid_character_name(""), "empty rejected");
    check(!valid_character_name(std::string("Bo\0b", 4)), "NUL rejected");
}

void checkMovement() {
    MoveRules rules; // 10 m/s + 1 tolerance, 1 s burst, y [-100, 500]; map bounds: lua_checks checkMaps
    MoveBudget b{0, T0};
    proto::Vec3 from{100, 0, 100};

    proto::Vec3 to{101, 0, 100};
    check(validate_move(rules, b, from, to, 0, T0 + 100ms) == MoveVerdict::accepted, "1 m in 100 ms accepted");
    from = to;
    to = {102.5f, 0, 100}; // budget: 0.1 left + 1.1 refill = 1.2 m
    check(validate_move(rules, b, from, to, 0, T0 + 200ms) == MoveVerdict::too_fast, "1.5 m in 100 ms rejected");
    // within tolerance: 10.5 m/s sustained for 10 moves
    MoveBudget b2{0, T0};
    proto::Vec3 p{500, 0, 500};
    bool allOk = true;
    for (int i = 1; i <= 10; ++i) {
        proto::Vec3 n{p.x + 1.05f, 0, p.z};
        allOk &= validate_move(rules, b2, p, n, 0, T0 + std::chrono::milliseconds(100 * i)) == MoveVerdict::accepted;
        p = n;
    }
    check(allOk, "10.5 m/s accepted within tolerance");
    // jitter: two moves arrive in the same instant after a pause are absorbed by the burst budget
    MoveBudget b3{0, T0};
    proto::Vec3 q{500, 0, 500}, q1{501, 0, 500}, q2{502, 0, 500};
    check(validate_move(rules, b3, q, q1, 0, T0 + 200ms) == MoveVerdict::accepted, "jitter move 1");
    check(validate_move(rules, b3, q1, q2, 0, T0 + 200ms) == MoveVerdict::accepted, "jitter move 2 same instant");
    // sustained speedhack: 30 m/s fails within the first second
    MoveBudget b4{0, T0};
    proto::Vec3 s{500, 0, 500};
    bool rejected = false;
    for (int i = 1; i <= 10 && !rejected; ++i) {
        proto::Vec3 n{s.x + 3, 0, s.z};
        if (validate_move(rules, b4, s, n, 0, T0 + std::chrono::milliseconds(100 * i)) == MoveVerdict::too_fast) rejected = true;
        else s = n;
    }
    check(rejected, "30 m/s rejected");
    // teleport
    MoveBudget b5{0, T0};
    proto::Vec3 tp{1000, 0, 1000};
    check(validate_move(rules, b5, {0, 0, 0}, tp, 0, T0 + 10s) == MoveVerdict::too_fast, "teleport rejected even after idle");
    // y clamp, not counted in speed (falling from 9000 m)
    MoveBudget b7{0, T0};
    proto::Vec3 high{10, 8000, 10};
    check(validate_move(rules, b7, {10, 9000, 10}, high, 0, T0 + 1s) == MoveVerdict::accepted && high.y == 500,
          "y clamped to MAX_Y, vertical not counted as speed");

    // ground at 10 m: groundTolerance 0.5, maxAir 6
    const float g = 10;
    MoveBudget b8{0, T0};
    proto::Vec3 at{10, g, 10}, under{11, g - 3, 10};
    check(validate_move(rules, b8, at, under, g, T0 + 1s) == MoveVerdict::accepted && under.y == g,
          "3 m under the ground: accepted, lifted onto it");
    proto::Vec3 slightly{12, g - 0.4f, 10};
    check(validate_move(rules, b8, under, slightly, g, T0 + 1s) == MoveVerdict::accepted && slightly.y == g - 0.4f,
          "0.4 m under (within groundTolerance): accepted as-is");
    proto::Vec3 edge{13, g + 6, 10};
    check(validate_move(rules, b8, slightly, edge, g, T0 + 2s) == MoveVerdict::accepted && edge.y == g + 6,
          "rising to exactly ground + maxAir: accepted");
    MoveBudget b9{0, T0};
    proto::Vec3 fly{11, g + 6.5f, 10};
    check(validate_move(rules, b9, at, fly, g, T0 + 1s) == MoveVerdict::in_air && fly.y == g + 6.5f,
          "rising past ground + maxAir: in_air");
    check(b9.meters > 10.9f, "in_air spends no budget");
    proto::Vec3 hoverFrom{10, g + 20, 10}, hover{11, g + 20, 10};
    check(validate_move(rules, b9, hoverFrom, hover, g, T0 + 1s) == MoveVerdict::in_air, "level flight 20 m up: in_air");
    proto::Vec3 sky{10, g + 50, 10}, falling{11, g + 49, 10};
    check(validate_move(rules, b9, sky, falling, g, T0 + 1s) == MoveVerdict::accepted && falling.y == g + 49,
          "falling from 50 m above the ground: accepted");
    proto::Vec3 down{12, g + 20, 10};
    check(validate_move(rules, b9, falling, down, g, T0 + 2s) == MoveVerdict::accepted, "still falling: accepted");
}

void checkAoi() {
    AoiGrid grid(64, 128);
    std::map<uint32_t, std::pair<float, float>> pos = {{1, {100.f, 100.f}}, {2, {200.f, 100.f}}, {3, {100.f, 229.f}}, {4, {500.f, 500.f}}};
    for (auto& [id, p] : pos) grid.insert(id, grid.cell_of(1, p.first, p.second));
    auto posOf = [&](uint32_t id, float& x, float& z) { x = pos[id].first; z = pos[id].second; };

    auto seen = grid.query(1, 100, 100, 1, posOf);
    check(seen == std::vector<uint32_t>{2}, "1 sees 2 (100 m) not 3 (129 m) or 4");
    check(grid.query(1, 200, 100, 2, posOf) == std::vector<uint32_t>{1}, "visibility is symmetric");

    // 3 walks into range, 2 walks out
    grid.remove(3, grid.cell_of(1, 100, 229));
    pos[3] = {100.f, 220.f};
    grid.insert(3, grid.cell_of(1, 100, 220));
    grid.remove(2, grid.cell_of(1, 200, 100));
    pos[2] = {300.f, 100.f};
    grid.insert(2, grid.cell_of(1, 300, 100));
    auto now = grid.query(1, 100, 100, 1, posOf);
    std::vector<uint32_t> entered, left;
    diff_sorted(seen, now, entered, left);
    check(entered == std::vector<uint32_t>{3}, "3 entered");
    check(left == std::vector<uint32_t>{2}, "2 left");
    check(grid.query(1, 1900, 1900, 0, posOf).empty(), "empty corner");
    // maps: same x/z on another map is another cell; negative cells and big map ids keep apart
    pos[5] = {100.f, 100.f};
    grid.insert(5, grid.cell_of(2, 100, 100));
    check(grid.query(1, 100, 100, 1, posOf) == std::vector<uint32_t>{3}, "map 1 never sees map 2's entity at the same x/z");
    check(grid.query(2, 100, 100, 0, posOf) == std::vector<uint32_t>{5}, "map 2 sees only its own");
    pos[6] = {-100.f, -100.f};
    grid.insert(6, grid.cell_of(65535, -100, -100));
    check(grid.query(65535, -100, -100, 0, posOf) == std::vector<uint32_t>{6} && grid.query(0, -100, -100, 0, posOf).empty(),
          "negative cells and map 65535 keyed apart");
}

void checkChat() {
    check(sanitize_chat("hello", 256) == std::optional<std::string>("hello"), "plain text");
    check(sanitize_chat("a\nb\tc\x7f" "d", 256) == std::optional<std::string>("abcd"), "C0/DEL stripped");
    check(sanitize_chat("x\xC2\x85y", 256) == std::optional<std::string>("xy"), "C1 (U+0085) stripped");
    check(sanitize_chat("caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x98\x80", 256).has_value(), "valid multibyte kept");
    check(!sanitize_chat("bad \xC3", 256), "truncated sequence rejected");
    check(!sanitize_chat("bad \xC0\xAF", 256), "overlong rejected");
    check(!sanitize_chat("bad \xED\xA0\x80", 256), "surrogate rejected");
    check(!sanitize_chat("bad \xFF", 256), "invalid byte rejected");
    check(!sanitize_chat("\n\t", 256), "only control chars rejected");
    check(!sanitize_chat("", 256), "empty rejected");
    check(sanitize_chat(std::string(256, 'a'), 256).has_value(), "256 bytes ok");
    check(!sanitize_chat(std::string(257, 'a'), 256), "257 bytes rejected");

    WindowLimiter chat(5, 10s);
    int allowed = 0;
    for (int i = 0; i < 7; ++i) allowed += chat.allow(T0 + std::chrono::milliseconds(i * 100));
    check(allowed == 5, "5 per 10 s");
    check(!chat.allow(T0 + 9s), "still limited at 9 s");
    check(chat.allow(T0 + 10s), "first slot frees at 10 s");
}

// ClientEvent (kast.world.send): the name and payload checks GameServer applies before World sees the event.
void checkClientEvents() {
    using proto::ClientEvent;
    check(proto::valid_client_event_name("combat.attack_2"), "event name a-z 0-9 . _ ok");
    check(proto::valid_client_event_name(std::string(32, 'a')), "32-char event name ok");
    check(!proto::valid_client_event_name(std::string(33, 'a')), "33-char event name rejected");
    check(!proto::valid_client_event_name(""), "empty event name rejected");
    for (const char* bad : {"Combat", "a b", "a-b", "a/b", "\xC3\xA9", "a\n"})
        check(!proto::valid_client_event_name(bad), "event name rejected");

    const auto data = client_event_data(ClientEvent{"combat.attack", R"({"target":12})"});
    check(data && (*data)["target"] == 12, "valid ClientEvent -> its JSON object");
    check(client_event_data(ClientEvent{"x", "{}"}).has_value(), "empty object ok");
    check(!client_event_data(ClientEvent{"Bad", "{}"}), "bad name rejected");
    check(!client_event_data(ClientEvent{"x", "[1,2]"}), "array payload rejected");
    check(!client_event_data(ClientEvent{"x", "5"}), "number payload rejected");
    check(!client_event_data(ClientEvent{"x", "{not json"}), "invalid JSON rejected");
    check(!client_event_data(ClientEvent{"x", ""}), "empty payload rejected");
    const std::string atLimit = R"({"s":")" + std::string(proto::kClientEventJsonMax - 8, 'a') + R"("})";
    check(atLimit.size() == 1024 && client_event_data(ClientEvent{"x", atLimit}).has_value(), "1024-byte payload ok");
    const std::string over = R"({"s":")" + std::string(proto::kClientEventJsonMax - 7, 'a') + R"("})";
    check(!client_event_data(ClientEvent{"x", over}), "1025-byte payload rejected");
}

void checkMessageRate() {
    MessageRateLimiter rl(60, 120, 2000ms);
    int accepted = 0;
    for (int i = 0; i < 120; ++i) accepted += rl.check(T0) == MessageRateLimiter::Verdict::accept;
    check(accepted == 120, "burst of 120 accepted");
    check(rl.check(T0) == MessageRateLimiter::Verdict::drop, "121st dropped");
    check(rl.check(T0 + 1999ms) != MessageRateLimiter::Verdict::disconnect, "brief excess tolerated");
    // flood continues: bucket refills 60/s but sender exceeds it; 2 s of continuous excess disconnects
    MessageRateLimiter flood(60, 120, 2000ms);
    MessageRateLimiter::Verdict v = MessageRateLimiter::Verdict::accept;
    int ms = 0;
    for (; ms < 10000 && v != MessageRateLimiter::Verdict::disconnect; ++ms) v = flood.check(T0 + std::chrono::milliseconds(ms));
    check(v == MessageRateLimiter::Verdict::disconnect, "1000 msg/s flood disconnects");
    // steady 50 msg/s never disconnects
    MessageRateLimiter steady(60, 120, 2000ms);
    bool anyBad = false;
    for (int i = 0; i < 500; ++i) anyBad |= steady.check(T0 + std::chrono::milliseconds(i * 20)) != MessageRateLimiter::Verdict::accept;
    check(!anyBad, "50 msg/s always accepted");
}

void checkConfig() {
    std::map<std::string, std::string> env = {{"AUTH_JWKS_URL", "http://auth:3000/.well-known/jwks.json"},
                                              {"DB_HOST", "mysql"}, {"DB_USER", "u"}, {"DB_PASSWORD", "p"},
                                              {"DB_NAME", "kast_world"}, {"REALM_ID", "1"}};
    auto lookup = [&](const char* n) -> std::optional<std::string> {
        auto it = env.find(n);
        return it == env.end() ? std::nullopt : std::optional(it->second);
    };
    auto ok = load_config(lookup);
    check(ok.errors.empty(), "minimal config valid");
    check(ok.config.mapRoot == "/app/maps" && ok.config.startMap == 1 && ok.config.port == 7777, "defaults applied");
    for (const char* bad : {"48", "100", "0.5"}) {
        env["AOI_CELL_SIZE"] = bad;
        const auto r = load_config(lookup);
        check(r.errors.size() == 1 && r.errors[0].starts_with("AOI_CELL_SIZE"), "AOI_CELL_SIZE must divide 256");
    }
    env["AOI_CELL_SIZE"] = "128";
    check(load_config(lookup).errors.empty(), "AOI_CELL_SIZE 128 divides 256");
    env.erase("AOI_CELL_SIZE");
    env["START_MAP"] = "65536";
    check(load_config(lookup).errors.size() == 1, "START_MAP above 65535 refused");
    env["START_MAP"] = "2";
    env["MAP_ROOT"] = "/maps";
    check(load_config(lookup).config.startMap == 2 && load_config(lookup).config.mapRoot == "/maps", "START_MAP/MAP_ROOT read");
    env.erase("START_MAP");
    env.erase("MAP_ROOT");
    check(ok.config.gameStateMachines.empty(), "GAME_STATE_MACHINES unset: no game machines");
    env["GAME_STATE_MACHINES"] = " zone_cycle ,, quest_flow,\t";
    check(load_config(lookup).config.gameStateMachines == std::vector<std::string>{"zone_cycle", "quest_flow"},
          "GAME_STATE_MACHINES: comma list, trimmed, empty entries ignored");
    env["GAME_STATE_MACHINES"] = "";
    check(load_config(lookup).config.gameStateMachines.empty(), "GAME_STATE_MACHINES empty: none");
    check(ok.config.groundTolerance == 0.5f && ok.config.maxAir == 6, "MOVE_GROUND_TOLERANCE / MOVE_MAX_AIR default 0.5 / 6");
    env["MOVE_GROUND_TOLERANCE"] = "1";
    env["MOVE_MAX_AIR"] = "2.5";
    const auto ground = load_config(lookup);
    check(ground.errors.empty() && ground.config.groundTolerance == 1 && ground.config.maxAir == 2.5f,
          "MOVE_GROUND_TOLERANCE / MOVE_MAX_AIR read");
    env["MOVE_MAX_AIR"] = "-1";
    const auto negative = load_config(lookup);
    check(negative.errors.size() == 1 && negative.errors[0].starts_with("MOVE_MAX_AIR"), "negative MOVE_MAX_AIR refused");
    env.erase("MOVE_GROUND_TOLERANCE");
    env.erase("MOVE_MAX_AIR");
    check(ok.config.maxNpcs == 10000, "MAX_NPCS defaults to 10000");
    env["MAX_NPCS"] = "0";
    check(load_config(lookup).errors.empty() && load_config(lookup).config.maxNpcs == 0, "MAX_NPCS 0: no NPCs");
    for (const char* badNpcs : {"-1", "100001", "many", "1.5"}) {
        env["MAX_NPCS"] = badNpcs;
        const auto r = load_config(lookup);
        check(r.errors.size() == 1 && r.errors[0].starts_with("MAX_NPCS"), "MAX_NPCS out of range or not a whole number refused");
    }

    env.erase("MAX_NPCS");
    check(ok.config.simLatencyMs == 0 && ok.config.simJitterMs == 0, "SIM_LATENCY_MS / SIM_JITTER_MS default 0 (lag simulator off)");
    env["SIM_LATENCY_MS"] = "150";
    env["SIM_JITTER_MS"] = "50";
    const auto lag = load_config(lookup);
    check(lag.errors.empty() && lag.config.simLatencyMs == 150 && lag.config.simJitterMs == 50, "SIM_LATENCY_MS / SIM_JITTER_MS read");
    env["SIM_JITTER_MS"] = "-5";
    check(load_config(lookup).errors.size() == 1, "negative SIM_JITTER_MS refused");
    env.erase("SIM_LATENCY_MS");
    env.erase("SIM_JITTER_MS");

    env.erase("DB_HOST");
    env["REALM_ID"] = "abc";
    env["MAX_FRAME_BYTES"] = "999999";
    env["LOG_LEVEL"] = "loud";
    env["TLS_CERT_FILE"] = "/x.pem";
    env["MAX_NPCS"] = "lots";
    auto bad = load_config(lookup);
    check(bad.errors.size() == 6, "every bad variable reported, MAX_NPCS with the others");
}

// The lag simulator's schedule (net/session.h lag_due): latency +- jitter, never reordering, never in the past.
void checkLagSim() {
    std::minstd_rand rng(1);
    TimePoint previous = T0;
    bool ordered = true, inRange = true, spread = false;
    Millis first{-1};
    for (int i = 0; i < 1000; ++i) {
        const TimePoint now = T0 + Millis(i); // one message a millisecond: jitter would reorder them
        const TimePoint due = net::lag_due(now, previous, Millis(150), Millis(50), rng);
        ordered &= due >= previous;
        const auto delay = std::chrono::duration_cast<Millis>(due - now);
        inRange &= delay >= Millis(100) && delay <= Millis(200); // waiting for the previous one never exceeds it either
        if (first.count() < 0) first = delay;
        spread |= delay != first;
        previous = due;
    }
    check(ordered && inRange && spread, "lag sim: delays of 150 +- 50 ms, in order");
    check(net::lag_due(T0, T0, Millis(0), Millis(0), rng) == T0, "lag sim: 0 / 0 delivers at once");
    std::minstd_rand fixed(7);
    bool exact = true;
    for (int i = 0; i < 100; ++i) exact &= net::lag_due(T0 + Millis(i), T0, Millis(150), Millis(0), fixed) == T0 + Millis(i + 150);
    check(exact, "lag sim: no jitter -> exactly the latency");
}

void checkTickStats() {
    TickStats s;
    for (int i = 1; i <= 100; ++i) s.add(i * 0.1); // 0.1 .. 10 ms
    check(s.count() == 100, "count");
    check(s.percentile(0.5) >= 5.0 && s.percentile(0.5) <= 5.02, "p50 ~5 ms");
    check(s.percentile(0.99) >= 9.9 && s.percentile(0.99) <= 9.92, "p99 ~9.9 ms");
    check(s.max() > 9.99 && s.max() < 10.01, "max");
}

// --- JWT ---

struct RsaKey {
    std::string privPem, pubPem, n, e;
};

std::string bio_string(BIO* bio) {
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<size_t>(len));
}

std::string b64url_bn(const BIGNUM* bn) {
    std::string bin(static_cast<size_t>(BN_num_bytes(bn)), '\0');
    BN_bn2bin(bn, reinterpret_cast<unsigned char*>(bin.data()));
    return jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(bin));
}

RsaKey make_rsa_key() {
    RsaKey k;
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    BIO* priv = BIO_new(BIO_s_mem());
    BIO* pub = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(priv, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    PEM_write_bio_PUBKEY(pub, pkey);
    k.privPem = bio_string(priv);
    k.pubPem = bio_string(pub);
    BIGNUM *n = nullptr, *e = nullptr;
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);
    k.n = b64url_bn(n);
    k.e = b64url_bn(e);
    BN_free(n);
    BN_free(e);
    BIO_free(priv);
    BIO_free(pub);
    EVP_PKEY_free(pkey);
    return k;
}

std::string b64url(const std::string& s) {
    return jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(s));
}

void checkJwt() {
    const RsaKey key = make_rsa_key();
    const RsaKey other = make_rsa_key();
    const SysTime now = std::chrono::system_clock::now();
    const JwtRules rules{"kast-auth", "kast", 5};

    const std::string jwks = nlohmann::json{{"keys", {{{"kty", "RSA"}, {"kid", "k1"}, {"alg", "RS256"}, {"use", "sig"},
                                                       {"n", key.n}, {"e", key.e}}}}}.dump();
    JwksCache cache([&] { return std::optional(jwks); }, 300s, 10s);
    const TimePoint t = T0;
    auto keyFor = [&](const std::string& kid) { return cache.key(kid, t); };

    auto token = [&](auto mutate) {
        auto b = jwt::create()
                     .set_key_id("k1")
                     .set_issuer("kast-auth")
                     .set_audience("kast")
                     .set_subject("42")
                     .set_payload_claim("email", jwt::claim(std::string("a@b.c")))
                     .set_payload_claim("role", jwt::claim(std::string("gm")))
                     .set_issued_at(now)
                     .set_expires_at(now + 15min);
        mutate(b);
        return b.sign(jwt::algorithm::rs256(key.pubPem, key.privPem));
    };
    auto none = [](auto&) {};

    auto valid = verify_access_token(token(none), keyFor, rules, now);
    check(valid.code == proto::Code::ok && valid.accountId == 42 && valid.role == proto::Role::gm, "valid token");

    auto expired = verify_access_token(token([&](auto& b) { b.set_expires_at(now - 60s); }), keyFor, rules, now);
    check(expired.code == proto::Code::token_expired, "expired token");
    auto skew = verify_access_token(token([&](auto& b) { b.set_expires_at(now - 3s); }), keyFor, rules, now);
    check(skew.code == proto::Code::ok, "expired 3 s ago is within skew");

    check(verify_access_token(token([](auto& b) { b.set_issuer("evil"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "wrong iss");
    check(verify_access_token(token([](auto& b) { b.set_audience("other"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "wrong aud");
    check(verify_access_token(token([](auto& b) { b.set_subject("abc"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "non-numeric sub");
    check(verify_access_token(token([](auto& b) { b.set_payload_claim("role", jwt::claim(std::string("god"))); }),
                              keyFor, rules, now).code == proto::Code::invalid_token, "unknown role");

    // alg none
    const std::string header = R"({"alg":"none","typ":"JWT","kid":"k1"})";
    const std::string payload = nlohmann::json{{"sub", "42"}, {"role", "admin"}, {"iss", "kast-auth"}, {"aud", "kast"},
                                               {"exp", std::chrono::system_clock::to_time_t(now + 15min)}}.dump();
    check(verify_access_token(b64url(header) + "." + b64url(payload) + ".", keyFor, rules, now).code ==
              proto::Code::invalid_token, "alg none");

    // HS256 signed with the public key as the HMAC secret (classic key-confusion attack)
    const auto hs = jwt::create().set_key_id("k1").set_issuer("kast-auth").set_audience("kast").set_subject("42")
                        .set_payload_claim("role", jwt::claim(std::string("admin")))
                        .set_expires_at(now + 15min).sign(jwt::algorithm::hs256(key.pubPem));
    check(verify_access_token(hs, keyFor, rules, now).code == proto::Code::invalid_token, "HS256");

    check(verify_access_token(token([](auto& b) { b.set_key_id("k9"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "unknown kid");
    const auto forged = jwt::create().set_key_id("k1").set_issuer("kast-auth").set_audience("kast").set_subject("42")
                            .set_payload_claim("role", jwt::claim(std::string("admin")))
                            .set_expires_at(now + 15min).sign(jwt::algorithm::rs256(other.pubPem, other.privPem));
    check(verify_access_token(forged, keyFor, rules, now).code == proto::Code::invalid_token, "signed by another key");
    check(verify_access_token("not.a.token", keyFor, rules, now).code == proto::Code::invalid_token, "garbage");
    check(verify_access_token("", keyFor, rules, now).code == proto::Code::invalid_token, "empty");
}

void checkJwksCache() {
    const RsaKey key = make_rsa_key();
    auto doc = [&](const char* kid) {
        return nlohmann::json{{"keys", {{{"kty", "RSA"}, {"kid", kid}, {"n", key.n}, {"e", key.e}}}}}.dump();
    };
    int fetches = 0;
    std::optional<std::string> body = doc("k1");
    JwksCache cache([&] { ++fetches; return body; }, 300s, 10s);

    check(cache.key("k1", T0).status == KeyStatus::ok && fetches == 1, "first lookup fetches");
    check(cache.key("k1", T0 + 299s).status == KeyStatus::ok && fetches == 1, "cached for 300 s");
    check(cache.key("k1", T0 + 300s).status == KeyStatus::ok && fetches == 2, "refreshed after 300 s");

    body = doc("k2"); // rotation
    check(cache.key("k2", T0 + 320s).status == KeyStatus::ok && fetches == 3, "unknown kid refetches once");
    check(cache.key("k9", T0 + 321s).status == KeyStatus::unknown_kid && fetches == 3,
          "unknown kid refetch is rate limited");
    check(cache.key("k9", T0 + 331s).status == KeyStatus::unknown_kid && fetches == 4, "refetch again after interval");

    body.reset(); // auth down
    check(cache.key("k2", T0 + 700s).status == KeyStatus::unavailable, "fetch failure -> unavailable");
    body = "{not json";
    check(cache.key("k2", T0 + 701s).status == KeyStatus::unavailable, "bad JWKS -> unavailable");
    check(!JwksCache::parse(R"({"nokeys":1})"), "JWKS without keys rejected");
}

} // namespace

int lua_checks(int& checks); // lua_checks.cpp

// A test never waits for a click: abort(), failed asserts, debug-CRT reports (STL debug checks) and crashes write to
// stderr and end the process instead of opening a modal dialog. Same as engine/crash_dialogs.cpp, without its
// KAST_NO_CRASH_DIALOGS switch.
static void disable_crash_dialogs() {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort(): no "Debug Error!" box, no WER report
    _set_error_mode(_OUT_TO_STDERR);                               // assert() in a release build
#ifdef _DEBUG
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // access violations: no "has stopped working" box
#endif
}

int main() {
    disable_crash_dialogs();
    checkCodecRoundTrip();
    checkFrameRejection();
    checkNames();
    checkMovement();
    checkAoi();
    checkChat();
    checkClientEvents();
    checkMessageRate();
    checkConfig();
    checkLagSim();
    checkTickStats();
    checkJwt();
    checkJwksCache();
    g_failures += lua_checks(g_checks);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
