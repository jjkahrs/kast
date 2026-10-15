// Assert-style checks for the world server's pure logic: frame codec (every message), frame rejection, names,
// movement, AOI sets, chat sanitizing and rate limits, config validation, tick stats and JWT/JWKS verification with
// locally generated RSA keys. No network, database or sleeping: time is passed in.
// Exit 0 if every check passes. Run by CTest as world_self_check.
#include "auth/jwks_cache.h"
#include "auth/jwt_verify.h"
#include "core/config.h"
#include "core/rate_limiter.h"
#include "core/tick_stats.h"
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
    roundtrip(CharDelete{1234567890123ull}, "CharDelete");
    roundtrip(EnterWorld{99}, "EnterWorld");
    roundtrip(Move{{1.5f, -2.25f, 1000.f}, 3.14f, 123456}, "Move");
    roundtrip(Say{"hello \xC3\xA9"}, "Say");
    roundtrip(AuthOk{77, Role::gm}, "AuthOk");
    roundtrip(Disconnect{Code::logged_in_elsewhere}, "Disconnect");
    roundtrip(CharList{{{1, "Alice", {1, 2, 3}}, {2, "Bob", {4, 5, 6}}}}, "CharList");
    roundtrip(CharList{}, "CharList empty");
    roundtrip(CharCreated{{5, "Carol", {7, 8, 9}}}, "CharCreated");
    roundtrip(CharDeleted{5}, "CharDeleted");
    roundtrip(EnterWorldOk{11, {1, 0, 1}, 1.f}, "EnterWorldOk");
    roundtrip(CharError{Opcode::CharCreate, Code::name_taken}, "CharError");
    roundtrip(Spawn{3, "Dave", {10, 0, 10}, 0.5f}, "Spawn");
    roundtrip(Despawn{3}, "Despawn");
    roundtrip(EntityMove{3, {11, 0, 12}, 0.25f}, "EntityMove");
    roundtrip(Correction{{1, 2, 3}}, "Correction");
    roundtrip(ChatMsg{"Eve", "hi all"}, "ChatMsg");

    // exact byte layout: u16 len, u16 opcode, payload, little-endian
    const auto f = encode(Message{Hello{0x0102}});
    check(f == std::vector<uint8_t>{2, 0, 1, 0, 0x02, 0x01}, "Hello bytes little-endian");

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
    MoveRules rules; // 10 m/s + 1 tolerance, 1 s burst, 2048x2048, y [-100, 500]
    MoveBudget b{0, T0};
    proto::Vec3 from{100, 0, 100};

    proto::Vec3 to{101, 0, 100};
    check(validate_move(rules, b, from, to, T0 + 100ms) == MoveVerdict::accepted, "1 m in 100 ms accepted");
    from = to;
    to = {102.5f, 0, 100}; // budget: 0.1 left + 1.1 refill = 1.2 m
    check(validate_move(rules, b, from, to, T0 + 200ms) == MoveVerdict::too_fast, "1.5 m in 100 ms rejected");
    // within tolerance: 10.5 m/s sustained for 10 moves
    MoveBudget b2{0, T0};
    proto::Vec3 p{500, 0, 500};
    bool allOk = true;
    for (int i = 1; i <= 10; ++i) {
        proto::Vec3 n{p.x + 1.05f, 0, p.z};
        allOk &= validate_move(rules, b2, p, n, T0 + std::chrono::milliseconds(100 * i)) == MoveVerdict::accepted;
        p = n;
    }
    check(allOk, "10.5 m/s accepted within tolerance");
    // jitter: two moves arrive in the same instant after a pause are absorbed by the burst budget
    MoveBudget b3{0, T0};
    proto::Vec3 q{500, 0, 500}, q1{501, 0, 500}, q2{502, 0, 500};
    check(validate_move(rules, b3, q, q1, T0 + 200ms) == MoveVerdict::accepted, "jitter move 1");
    check(validate_move(rules, b3, q1, q2, T0 + 200ms) == MoveVerdict::accepted, "jitter move 2 same instant");
    // sustained speedhack: 30 m/s fails within the first second
    MoveBudget b4{0, T0};
    proto::Vec3 s{500, 0, 500};
    bool rejected = false;
    for (int i = 1; i <= 10 && !rejected; ++i) {
        proto::Vec3 n{s.x + 3, 0, s.z};
        if (validate_move(rules, b4, s, n, T0 + std::chrono::milliseconds(100 * i)) == MoveVerdict::too_fast) rejected = true;
        else s = n;
    }
    check(rejected, "30 m/s rejected");
    // teleport
    MoveBudget b5{0, T0};
    proto::Vec3 tp{1000, 0, 1000};
    check(validate_move(rules, b5, {0, 0, 0}, tp, T0 + 10s) == MoveVerdict::too_fast, "teleport rejected even after idle");
    // bounds
    MoveBudget b6{0, T0};
    proto::Vec3 out1{-0.5f, 0, 1};
    check(validate_move(rules, b6, {0.5f, 0, 1}, out1, T0 + 1s) == MoveVerdict::out_of_bounds, "x < 0 rejected");
    proto::Vec3 out2{2047.5f, 0, 2048.5f};
    check(validate_move(rules, b6, {2047.5f, 0, 2047.9f}, out2, T0 + 2s) == MoveVerdict::out_of_bounds, "z > size rejected");
    proto::Vec3 edge{2048, 0, 2048};
    check(validate_move(rules, b6, {2047.5f, 0, 2047.5f}, edge, T0 + 3s) == MoveVerdict::accepted, "edge accepted");
    // y clamp, not counted in speed
    MoveBudget b7{0, T0};
    proto::Vec3 high{10, 9000, 10};
    check(validate_move(rules, b7, {10, 0, 10}, high, T0 + 1s) == MoveVerdict::accepted && high.y == 500,
          "y clamped to MAX_Y, vertical not counted as speed");
}

void checkAoi() {
    AoiGrid grid(64, 128);
    std::map<uint32_t, std::pair<float, float>> pos = {{1, {100.f, 100.f}}, {2, {200.f, 100.f}}, {3, {100.f, 229.f}}, {4, {500.f, 500.f}}};
    for (auto& [id, p] : pos) grid.insert(id, grid.cell_of(p.first, p.second));
    auto posOf = [&](uint32_t id, float& x, float& z) { x = pos[id].first; z = pos[id].second; };

    auto seen = grid.query(100, 100, 1, posOf);
    check(seen == std::vector<uint32_t>{2}, "1 sees 2 (100 m) not 3 (129 m) or 4");
    check(grid.query(200, 100, 2, posOf) == std::vector<uint32_t>{1}, "visibility is symmetric");

    // 3 walks into range, 2 walks out
    grid.remove(3, grid.cell_of(100, 229));
    pos[3] = {100.f, 220.f};
    grid.insert(3, grid.cell_of(100, 220));
    grid.remove(2, grid.cell_of(200, 100));
    pos[2] = {300.f, 100.f};
    grid.insert(2, grid.cell_of(300, 100));
    auto now = grid.query(100, 100, 1, posOf);
    std::vector<uint32_t> entered, left;
    diff_sorted(seen, now, entered, left);
    check(entered == std::vector<uint32_t>{3}, "3 entered");
    check(left == std::vector<uint32_t>{2}, "2 left");
    check(grid.query(1900, 1900, 0, posOf).empty(), "empty corner");
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
    check(ok.config.spawn_x() == 1024 && ok.config.port == 7777, "defaults applied");

    env.erase("DB_HOST");
    env["REALM_ID"] = "abc";
    env["MAX_FRAME_BYTES"] = "999999";
    env["LOG_LEVEL"] = "loud";
    env["TLS_CERT_FILE"] = "/x.pem";
    auto bad = load_config(lookup);
    check(bad.errors.size() == 5, "every bad variable reported");
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

int main() {
    checkCodecRoundTrip();
    checkFrameRejection();
    checkNames();
    checkMovement();
    checkAoi();
    checkChat();
    checkMessageRate();
    checkConfig();
    checkTickStats();
    checkJwt();
    checkJwksCache();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
