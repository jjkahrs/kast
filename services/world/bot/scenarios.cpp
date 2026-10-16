#include "scenarios.h"

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <cmath>
#include <fstream>
#include <sstream>

namespace kast::bot {
namespace {

using proto::Code;

struct Ctx {
    const Options& o;
    asio::ssl::context& tls;
    AuthClient auth;
    Checker check;

    // Real access token for the bot account `tag` (registers + verifies on first use).
    std::string token(const std::string& tag) {
        std::string err;
        auto t = auth.ensure_login(bot_account(o, tag), err);
        check.expect(t.has_value(), "access token for " + tag + (t ? "" : " (" + err + ")"));
        return t.value_or("");
    }

    asio::awaitable<std::shared_ptr<WorldClient>> login(const std::string& token, const std::string& who) {
        std::string err;
        auto c = co_await connect_and_login(o, tls, token, err);
        check.expect(c != nullptr, who + " TLS connect + AuthOk" + (c ? "" : " (" + err + ")"));
        co_return c;
    }

    // login + enter with a fresh random character name
    asio::awaitable<std::shared_ptr<WorldClient>> enter(const std::string& tag, proto::EnterWorldOk& ok) {
        auto c = co_await login(token(tag), tag);
        if (!c) co_return nullptr;
        std::string err;
        const bool in = co_await enter_world(*c, random_name(), ok, err);
        check.expect(in, tag + " entered world" + (in ? "" : " (" + err + ")"));
        co_return in ? c : nullptr;
    }
};

template <typename T>
std::vector<T> only(const std::vector<proto::Message>& msgs) {
    std::vector<T> out;
    for (const auto& m : msgs)
        if (auto* t = std::get_if<T>(&m)) out.push_back(*t);
    return out;
}

std::string fmt(const proto::Vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return buf;
}

bool same_pos(const proto::Vec3& a, const proto::Vec3& b) {
    return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f && std::fabs(a.z - b.z) < 0.01f;
}

// Waits for a CharError or the expected reply; returns the CharError code, or ok.
template <typename Reply>
asio::awaitable<Code> request(WorldClient& c, const proto::Message& m, std::optional<Reply>* reply = nullptr) {
    c.send(m);
    for (;;) {
        auto r = co_await c.next(Millis(10000));
        if (!r) co_return Code::malformed; // no answer
        if (auto* e = std::get_if<proto::CharError>(&*r)) co_return e->code;
        if (auto* ok = std::get_if<Reply>(&*r)) {
            if (reply) *reply = *ok;
            co_return Code::ok;
        }
    }
}

// --- 1 ---
asio::awaitable<void> auth_handshake(Ctx& x) {
    auto c = co_await x.login(x.token("s1"), "s1");
    if (!c) co_return;
    x.check.expect(c->authOk->accountId > 0, "AuthOk carries account id " + std::to_string(c->authOk->accountId));
    x.check.expect(c->authOk->role == proto::Role::player, "AuthOk role is player");
    c->close();
}

// --- 2 ---
asio::awaitable<void> characters(Ctx& x) {
    auto c = co_await x.login(x.token("s2"), "s2");
    if (!c) co_return;
    std::vector<std::string> names;
    bool allCreated = true;
    for (int i = 0; i < 10; ++i) {
        names.push_back(random_name());
        std::optional<proto::CharCreated> r;
        allCreated &= co_await request(*c, proto::CharCreate{names.back()}, &r) == Code::ok && r &&
                      r->character.name == names.back();
    }
    x.check.expect(allCreated, "10 characters created");
    x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{random_name()}) == Code::char_limit,
                   "11th character -> char_limit");

    std::optional<proto::CharList> list;
    co_await request(*c, proto::CharListReq{}, &list);
    x.check.expect(list && list->chars.size() == 10, "CharList has 10");

    std::optional<proto::CharDeleted> del;
    const uint64_t victim = list ? list->chars.front().id : 0;
    x.check.expect(co_await request(*c, proto::CharDelete{victim}, &del) == Code::ok, "delete one character");
    co_await request(*c, proto::CharListReq{}, &list);
    x.check.expect(list && list->chars.size() == 9, "CharList has 9 after delete");

    std::string lower = names[1];
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{lower}) == Code::name_taken,
                   "duplicate name in other case -> name_taken");
    x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{names[0]}) == Code::name_taken,
                   "deleted character's name stays reserved -> name_taken");
    for (const char* bad : {"ab", "Abcdefghijklmnopq", "Bob1", "Bo b", "B\xC3\xA9o", ""})
        x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{bad}) == Code::name_invalid,
                       std::string("invalid name \"") + bad + "\" -> name_invalid");
    x.check.expect(co_await request<proto::CharDeleted>(*c, proto::CharDelete{victim}) == Code::not_found,
                   "deleting an already deleted character -> not_found");

    // another account can't take the name either (realm-wide uniqueness)
    auto other = co_await x.login(x.token("s2b"), "s2b");
    if (other) {
        std::string upper = names[2];
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        x.check.expect(co_await request<proto::CharCreated>(*other, proto::CharCreate{upper}) == Code::name_taken,
                       "same name from another account -> name_taken");
        other->close();
    }
    c->close();
}

// --- 3 ---
asio::awaitable<void> ownership(Ctx& x) {
    auto a = co_await x.login(x.token("s3a"), "s3a");
    auto b = co_await x.login(x.token("s3b"), "s3b");
    if (!a || !b) co_return;
    std::optional<proto::CharCreated> created;
    co_await request(*a, proto::CharCreate{random_name()}, &created);
    x.check.expect(created.has_value(), "A created a character");
    if (!created) co_return;
    const uint64_t id = created->character.id;
    x.check.expect(co_await request<proto::EnterWorldOk>(*b, proto::EnterWorld{id}) == Code::not_found,
                   "B entering A's character -> not_found");
    x.check.expect(co_await request<proto::CharDeleted>(*b, proto::CharDelete{id}) == Code::not_found,
                   "B deleting A's character -> not_found");
    std::optional<proto::CharList> list;
    co_await request(*a, proto::CharListReq{}, &list);
    x.check.expect(list && list->chars.size() == 1 && list->chars[0].id == id, "A's character is untouched");
    std::optional<proto::EnterWorldOk> ok;
    x.check.expect(co_await request(*a, proto::EnterWorld{id}, &ok) == Code::ok, "A can enter its own character");
    a->close();
    b->close();
}

// --- 4 ---
asio::awaitable<void> aoi(Ctx& x) {
    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter("s4a", ea);
    auto b = co_await x.enter("s4b", eb);
    if (!a || !b) co_return;
    auto sa = co_await a->wait_for<proto::Spawn>(Millis(3000));
    auto sb = co_await b->wait_for<proto::Spawn>(Millis(3000));
    x.check.expect(sa && sa->entityId == eb.entityId, "A sees Spawn for B");
    x.check.expect(sb && sb->entityId == ea.entityId && !sb->name.empty(), "B sees Spawn for A (" + (sb ? sb->name : "") + ")");

    auto p = co_await walk(*a, ea.pos, 20, 0);
    auto bMsgs = co_await drain(*b, Millis(500));
    auto aMsgs = co_await drain(*a, Millis(100));
    auto moves = only<proto::EntityMove>(bMsgs);
    const bool sawA = !moves.empty() && moves.back().entityId == ea.entityId && same_pos(moves.back().pos, p);
    x.check.expect(sawA, "B received A's moves, last at " + fmt(p) + " (" + std::to_string(moves.size()) + " updates)");
    bool echo = false;
    for (auto& m : only<proto::EntityMove>(aMsgs)) echo |= m.entityId == ea.entityId;
    x.check.expect(!echo, "A never receives its own moves");

    co_await walk(*a, p, 130, 0); // 150 m from B: past the 128 m view radius
    auto late = co_await drain(*b, Millis(500));
    bool despawned = false;
    for (auto& d : only<proto::Despawn>(late)) despawned |= d.entityId == ea.entityId;
    x.check.expect(despawned, "B gets Despawn when A walks out of range");
    a->close();
    b->close();
}

// --- 5 ---
asio::awaitable<void> chat(Ctx& x) {
    proto::EnterWorldOk ea, eb, ec;
    auto a = co_await x.enter("s5a", ea);
    auto b = co_await x.enter("s5b", eb);
    auto c = co_await x.enter("s5c", ec);
    if (!a || !b || !c) co_return;
    co_await walk(*c, ec.pos, 0, 150); // C leaves range
    co_await drain(*a, Millis(300));
    co_await drain(*b, Millis(300));
    co_await drain(*c, Millis(300));

    a->send(proto::Say{"hello in range"});
    auto own = co_await a->wait_for<proto::ChatMsg>(Millis(2000));
    auto inRange = co_await b->wait_for<proto::ChatMsg>(Millis(2000));
    auto outRange = co_await c->wait_for<proto::ChatMsg>(Millis(1500));
    x.check.expect(own && own->text == "hello in range", "sender gets its own ChatMsg");
    x.check.expect(inRange && inRange->text == "hello in range" && own && inRange->fromName == own->fromName,
                   "B in range gets ChatMsg from " + (inRange ? inRange->fromName : std::string("?")));
    x.check.expect(!outRange, "C out of range gets nothing");

    a->send(proto::Say{"bad\x01\x1b[31m\ttext"});
    auto clean = co_await b->wait_for<proto::ChatMsg>(Millis(2000));
    x.check.expect(clean && clean->text == "bad[31mtext", "control characters stripped");
    a->send(proto::Say{std::string(257, 'x')});
    a->send(proto::Say{"bad \xC3"});

    for (int i = 0; i < 6; ++i) a->send(proto::Say{"spam " + std::to_string(i)});
    auto msgs = only<proto::ChatMsg>(co_await drain(*b, Millis(1500)));
    // the oversized and invalid ones still count: 4 used, so only 1 of the 6 spam messages gets through
    x.check.expect(msgs.size() == 1, "rate limit: 5 per 10 s (1 of 6 spam delivered; got " +
                                         std::to_string(msgs.size()) + ")");
    x.check.expect(!a->closed() && !b->closed(), "oversized/invalid chat is dropped, not fatal");
    a->close();
    b->close();
    c->close();
}

// --- 6 ---
asio::awaitable<void> speedhack(Ctx& x) {
    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter("s6a", ea);
    auto b = co_await x.enter("s6b", eb);
    if (!a || !b) co_return;
    co_await drain(*b, Millis(1500)); // spawns; also lets A's move budget fill
    const auto start = std::chrono::steady_clock::now();
    const proto::Vec3 origin = ea.pos;

    a->send(proto::Move{{origin.x + 100, origin.y, origin.z}, 0, 0}); // teleport
    auto corr = co_await a->wait_for<proto::Correction>(Millis(2000));
    x.check.expect(corr && same_pos(corr->pos, origin), "teleport -> Correction back to " + fmt(origin));

    proto::Vec3 p = origin;
    asio::steady_timer t(co_await asio::this_coro::executor);
    for (int i = 0; i < 30; ++i) { // 30 m/s
        t.expires_after(Millis(100));
        co_await t.async_wait(asio::use_awaitable);
        p.x += 3;
        a->send(proto::Move{p, 0, 0});
    }
    auto aMsgs = co_await drain(*a, Millis(500));
    auto bMsgs = co_await drain(*b, Millis(500));
    x.check.expect(!only<proto::Correction>(aMsgs).empty(), "sustained 30 m/s -> Correction");
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    float farthest = 0;
    for (auto& m : only<proto::EntityMove>(bMsgs))
        if (m.entityId == ea.entityId) farthest = std::max(farthest, distance_xz(m.pos, origin));
    const float allowed = static_cast<float>(11.0 * secs + 11.0); // (10 m/s + 1 tolerance) + 1 s burst
    x.check.expect(farthest <= allowed, "observer never sees an illegal position (farthest " + std::to_string(farthest) +
                                            " m <= " + std::to_string(allowed) + " m)");
    x.check.expect(farthest < 100, "observer never sees the teleport");
    a->close();
    b->close();
}

// --- 7 ---
asio::awaitable<void> malformed(Ctx& x) {
    proto::EnterWorldOk ew;
    auto watcher = co_await x.enter("s7w", ew);
    struct Case {
        const char* what;
        std::vector<uint8_t> bytes;
        Code expect;
    };
    const std::vector<Case> cases = {
        {"malformed Move (3-byte payload)", {3, 0, 0x20, 0x00, 1, 2, 3}, Code::malformed},
        {"oversized frame (5000 bytes)", {0x88, 0x13, 0x21, 0x00}, Code::frame_too_large},
        {"unknown opcode 0x7777", {0, 0, 0x77, 0x77}, Code::unknown_opcode},
        {"server-only opcode from client", proto::encode(proto::Message{proto::AuthOk{1, proto::Role::admin}}),
         Code::unexpected_message},
    };
    int i = 0;
    for (const auto& c : cases) {
        auto bad = co_await x.login(x.token("s7m" + std::to_string(i++)), c.what);
        if (!bad) continue;
        bad->send_raw(c.bytes);
        const bool closed = co_await bad->wait_closed(Millis(5000));
        x.check.expect(closed && bad->disconnectReason == c.expect,
                       std::string(c.what) + " -> " + proto::code_name(c.expect) + " (got " +
                           (bad->disconnectReason ? proto::code_name(*bad->disconnectReason) : "none") + ")");
    }
    if (!watcher) co_return;
    std::optional<proto::CharList> unused;
    const Code still = co_await request(*watcher, proto::CharListReq{}, &unused);
    x.check.expect(!watcher->closed() && still == Code::already_in_world, "other session unaffected (still answered)");
    watcher->close();
}

// --- 8 ---
std::string mint_expired_token(const Ctx& x, std::string& err) {
    std::ifstream in(x.o.authKeyFile);
    if (!in) { err = "cannot read --auth-key " + x.o.authKeyFile; return {}; }
    std::stringstream pem;
    pem << in.rdbuf();
    AuthClient auth(x.o.auth);
    auto jwks = auth.jwks(err);
    if (!jwks) return {};
    const std::string kid = nlohmann::json::parse(*jwks)["keys"][0]["kid"];
    const auto now = std::chrono::system_clock::now();
    return jwt::create()
        .set_key_id(kid)
        .set_issuer(x.o.extra.count("issuer") ? x.o.extra.at("issuer") : "kast-auth")
        .set_audience(x.o.extra.count("audience") ? x.o.extra.at("audience") : "kast")
        .set_subject("1")
        .set_payload_claim("role", jwt::claim(std::string("player")))
        .set_issued_at(now - std::chrono::minutes(20))
        .set_expires_at(now - std::chrono::minutes(5))
        .sign(jwt::algorithm::rs256("", pem.str()));
}

asio::awaitable<void> rejected_login(Ctx& x, const std::string& what, const std::string& token, uint16_t version,
                                     Code expect) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
    if (!co_await c->connect(x.o.host, x.o.port)) { x.check.expect(false, what + ": connect"); co_return; }
    co_await c->login(token, version);
    co_await c->wait_closed(Millis(5000));
    x.check.expect(!c->authOk && c->closed() && c->disconnectReason == expect,
                   what + " -> " + proto::code_name(expect) + " (got " +
                       (c->disconnectReason ? proto::code_name(*c->disconnectReason) : "none") + ")");
}

asio::awaitable<void> handshake_rejects(Ctx& x) {
    const std::string good = x.token("s8");
    co_await rejected_login(x, "garbage token", "not-a-jwt", proto::kProtocolVersion, Code::invalid_token);
    co_await rejected_login(x, "token with tampered signature", good.substr(0, good.size() - 4) + "AAAA",
                            proto::kProtocolVersion, Code::invalid_token);
    std::string err;
    const std::string expired = mint_expired_token(x, err);
    x.check.expect(!expired.empty(), "minted an expired token with auth's key" + (err.empty() ? "" : " (" + err + ")"));
    if (!expired.empty())
        co_await rejected_login(x, "expired token", expired, proto::kProtocolVersion, Code::token_expired);
    co_await rejected_login(x, "wrong protocol version", good, 999, Code::protocol_mismatch);

    {
        auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
        co_await c->connect(x.o.host, x.o.port);
        c->send(proto::Hello{});
        const auto t0 = std::chrono::steady_clock::now();
        co_await c->wait_closed(Millis(15000));
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        x.check.expect(c->disconnectReason == Code::handshake_timeout && secs > 8 && secs < 12,
                       "no Auth within 10 s -> handshake_timeout after " + std::to_string(secs) + " s");
    }
    {
        auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
        const bool connected = co_await c->connect(x.o.host, x.o.port, /*tls=*/false);
        const auto t0 = std::chrono::steady_clock::now();
        co_await c->wait_closed(Millis(15000));
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        x.check.expect(connected && c->closed() && secs > 8 && secs < 12,
                       "plain TCP without TLS is closed after " + std::to_string(secs) + " s");
    }
}

// --- 9 ---
asio::awaitable<void> duplicate_login(Ctx& x) {
    proto::EnterWorldOk e1;
    auto first = co_await x.enter("s9", e1);
    if (!first) co_return;
    const auto p = co_await walk(*first, e1.pos, 12, -7);
    co_await sleep_for(Millis(500));

    auto second = co_await x.login(x.token("s9"), "second login, same account");
    if (!second) co_return;
    co_await first->wait_closed(Millis(5000));
    x.check.expect(first->disconnectReason == Code::logged_in_elsewhere, "old session gets logged_in_elsewhere");
    proto::EnterWorldOk e2;
    std::string err;
    const bool in = co_await enter_world(*second, "unused", e2, err);
    x.check.expect(in && same_pos(e2.pos, p), "new session enters at the saved position " + fmt(p) + " (got " + fmt(e2.pos) + ")");
    second->close();
}

// --- 10 ---
asio::awaitable<void> persist_relogin(Ctx& x) {
    proto::EnterWorldOk e1;
    auto c = co_await x.enter("s10", e1);
    if (!c) co_return;
    const auto p = co_await walk(*c, e1.pos, -15, 9);
    co_await sleep_for(Millis(300));
    c->close();
    co_await sleep_for(Millis(500));
    proto::EnterWorldOk e2;
    auto again = co_await x.enter("s10", e2);
    x.check.expect(again && same_pos(e2.pos, p), "position kept across logout/login: " + fmt(p) + " (got " + fmt(e2.pos) + ")");
    if (again) again->close();
}

// Graceful restart, phase 1: enter, move, then wait for the server to shut down.
asio::awaitable<void> shutdown_phase1(Ctx& x) {
    proto::EnterWorldOk e;
    auto c = co_await x.enter(x.o.extra.count("tag") ? x.o.extra.at("tag") : "s12", e);
    if (!c) co_return;
    const auto p = co_await walk(*c, e.pos, 8, 13);
    co_await sleep_for(Millis(1000)); // last move processed
    std::printf("LASTPOS %.4f %.4f %.4f\nREADY\n", p.x, p.y, p.z);
    std::fflush(stdout);
    const bool closed = co_await c->wait_closed(Millis(60000));
    x.check.expect(closed && c->disconnectReason == Code::server_shutdown, "client gets Disconnect{server_shutdown}");
}

asio::awaitable<void> restart_verify(Ctx& x) {
    proto::Vec3 want{};
    std::istringstream in(x.o.expectPos);
    char c1 = 0, c2 = 0;
    in >> want.x >> c1 >> want.y >> c2 >> want.z;
    if (!in || c1 != ',' || c2 != ',') {
        x.check.expect(false, "--expect-pos x,y,z");
        co_return;
    }
    proto::EnterWorldOk e;
    auto c = co_await x.enter(x.o.extra.count("tag") ? x.o.extra.at("tag") : "s12", e);
    x.check.expect(c && same_pos(e.pos, want), "position survived the restart: " + fmt(want) + " (got " + fmt(e.pos) + ")");
    if (c) c->close();
}

// --- 11 ---
asio::awaitable<void> db_down(Ctx& x) {
    proto::EnterWorldOk ea;
    auto a = co_await x.enter("s11a", ea);
    auto b = co_await x.login(x.token("s11b"), "s11b");
    const std::string lateToken = x.token("s11c"); // auth needs the same MySQL: get it while the DB is up
    if (!a || !b) co_return;
    std::printf("READY stop-db\n");
    std::fflush(stdout);

    // Keep A walking in a square while B polls the character list until the DB is reported down.
    asio::steady_timer t(co_await asio::this_coro::executor);
    proto::Vec3 p = ea.pos;
    int step = 0, corrections = 0;
    bool sawUnavailable = false;
    auto move_a = [&] {
        static const float dirs[4][2] = {{0.9f, 0}, {0, 0.9f}, {-0.9f, 0}, {0, -0.9f}};
        const auto& d = dirs[(step / 50) % 4];
        p.x += d[0];
        p.z += d[1];
        a->send(proto::Move{p, 0, 0});
        ++step;
    };
    auto drain_a = [&]() -> asio::awaitable<void> {
        for (auto& m : co_await drain(*a, Millis(0))) corrections += std::holds_alternative<proto::Correction>(m);
    };
    for (int i = 0; i < 900 && !sawUnavailable; ++i) { // up to 90 s
        t.expires_after(Millis(100));
        co_await t.async_wait(asio::use_awaitable);
        move_a();
        co_await drain_a();
        if (i % 10 == 0) {
            std::optional<proto::CharList> l;
            sawUnavailable = co_await request(*b, proto::CharListReq{}, &l) == Code::service_unavailable;
        }
    }
    x.check.expect(sawUnavailable, "CharList -> service_unavailable while MySQL is down");
    x.check.expect(co_await request<proto::CharCreated>(*b, proto::CharCreate{random_name()}) == Code::service_unavailable,
                   "CharCreate -> service_unavailable while MySQL is down");
    {
        std::string err;
        auto late = co_await connect_and_login(x.o, x.tls, lateToken, err);
        x.check.expect(late != nullptr, "new auth handshake still works while MySQL is down");
        if (late) late->close();
    }
    for (int i = 0; i < 30; ++i) { // 3 more seconds in the world
        t.expires_after(Millis(100));
        co_await t.async_wait(asio::use_awaitable);
        move_a();
        co_await drain_a();
    }
    co_await sleep_for(Millis(300));
    x.check.expect(!a->closed() && corrections == 0, "in-world bot kept moving, no disconnect, no corrections");
    a->close(); // its save fails now and must be retried later
    std::printf("LASTPOS %.4f %.4f %.4f\nREADY start-db\n", p.x, p.y, p.z);
    std::fflush(stdout);

    bool back = false;
    for (int i = 0; i < 90 && !back; ++i) { // up to 3 min for MySQL to come back
        co_await sleep_for(Millis(2000));
        std::optional<proto::CharList> l;
        back = co_await request(*b, proto::CharListReq{}, &l) == Code::ok;
    }
    x.check.expect(back, "character requests work again after MySQL restarts");
    b->close();
    proto::EnterWorldOk e2;
    auto again = co_await x.enter("s11a", e2);
    x.check.expect(again && same_pos(e2.pos, p), "pending save landed: position " + fmt(p) + " (got " + fmt(e2.pos) + ")");
    if (again) again->close();
}

// --- 13: Lua components, with the sample lua/ (Health: aoi + persist, Wallet: owner + persist, systems/regen.lua) ---

std::vector<proto::ComponentUpdate> updates_for(const std::vector<proto::Message>& msgs, uint32_t entity,
                                                const std::string& name) {
    std::vector<proto::ComponentUpdate> out;
    for (const auto& u : only<proto::ComponentUpdate>(msgs))
        if (u.entityId == entity && u.name == name) out.push_back(u);
    return out;
}

nlohmann::json data_of(const proto::ComponentUpdate& u) { return nlohmann::json::parse(u.json, nullptr, false); }
double health_of(const proto::ComponentUpdate& u) { return data_of(u).value("current", -1.0); }
int gold_of(const proto::ComponentUpdate& u) { return data_of(u).value("gold", -1); }

// Index of the first message matching pred, or msgs.size().
template <typename Pred>
size_t first_index(const std::vector<proto::Message>& msgs, Pred pred) {
    for (size_t i = 0; i < msgs.size(); ++i)
        if (pred(msgs[i])) return i;
    return msgs.size();
}

// A and B in range: aoi Health reaches owner and observer, owner Wallet only the owner, Spawn before data,
// unchanged values not resent, nothing after Despawn; then A relogs and its Health and Wallet were persisted.
// Prints "COMPONENTS <gold> <health>" (A's last values) for components-verify after a restart.
asio::awaitable<void> components(Ctx& x) {
    const std::string tag = x.o.extra.count("tag") ? x.o.extra.at("tag") : "s13";
    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter(tag + "a", ea);
    auto b = co_await x.enter(tag + "b", eb);
    if (!a || !b) co_return;
    const uint32_t A = ea.entityId, B = eb.entityId;
    auto aMsgs = co_await drain(*a, Millis(1500));
    auto bMsgs = co_await drain(*b, Millis(100));

    const auto aHealth = updates_for(aMsgs, A, "Health"), aWallet = updates_for(aMsgs, A, "Wallet");
    x.check.expect(aHealth.size() == 1 && health_of(aHealth[0]) == 100.0, "A gets its own Health once (100, standing at max)");
    x.check.expect(aWallet.size() == 1 && gold_of(aWallet[0]) == 1, "A gets its own Wallet (gold 1: first login)");
    x.check.expect(updates_for(aMsgs, B, "Health").size() == 1, "A gets B's Health (aoi)");
    x.check.expect(updates_for(aMsgs, B, "Wallet").empty(), "A never gets B's Wallet (owner)");
    x.check.expect(updates_for(bMsgs, A, "Health").size() == 1, "B gets A's Health (aoi)");
    x.check.expect(updates_for(bMsgs, A, "Wallet").empty(), "B never gets A's Wallet (owner)");
    x.check.expect(updates_for(bMsgs, B, "Wallet").size() == 1, "B gets its own Wallet");
    const size_t spawnA = first_index(bMsgs, [&](const proto::Message& m) {
        auto* sp = std::get_if<proto::Spawn>(&m);
        return sp && sp->entityId == A;
    });
    const size_t dataA = first_index(bMsgs, [&](const proto::Message& m) {
        auto* u = std::get_if<proto::ComponentUpdate>(&m);
        return u && u->entityId == A;
    });
    x.check.expect(spawnA < dataA && dataA < bMsgs.size(), "B gets Spawn(A) before A's component data");

    // A walks: Health drains (sent to both); B stands at max, so nothing is resent for B.
    co_await walk(*a, ea.pos, 20, 0);
    co_await sleep_for(Millis(300));
    aMsgs = co_await drain(*a, Millis(200));
    bMsgs = co_await drain(*b, Millis(100));
    const auto walked = updates_for(bMsgs, A, "Health");
    x.check.expect(!walked.empty() && health_of(walked.back()) < 95.0,
                   "B sees A's Health drain while A moves (" + std::to_string(walked.size()) + " updates, last " +
                       (walked.empty() ? std::string("-") : std::to_string(health_of(walked.back()))) + ")");
    x.check.expect(updates_for(bMsgs, B, "Health").empty() && updates_for(aMsgs, B, "Health").empty(),
                   "B's unchanged Health is not resent");
    const auto ownSeen = updates_for(aMsgs, A, "Health");
    x.check.expect(!ownSeen.empty(), "A gets its own Health changes");
    const double before = ownSeen.empty() ? -1 : health_of(ownSeen.back());

    a->close();
    auto afterLeave = co_await drain(*b, Millis(1000));
    const size_t despawnA = first_index(afterLeave, [&](const proto::Message& m) {
        auto* d = std::get_if<proto::Despawn>(&m);
        return d && d->entityId == A;
    });
    bool lateData = false;
    for (size_t i = despawnA; i < afterLeave.size(); ++i)
        if (auto* u = std::get_if<proto::ComponentUpdate>(&afterLeave[i])) lateData |= u->entityId == A;
    x.check.expect(despawnA < afterLeave.size() && !lateData, "B gets Despawn(A) and no A component data after it");

    proto::EnterWorldOk e2;
    auto again = co_await x.enter(tag + "a", e2);
    if (!again) co_return;
    auto relog = co_await drain(*again, Millis(1000));
    const auto health = updates_for(relog, e2.entityId, "Health"), wallet = updates_for(relog, e2.entityId, "Wallet");
    const double first = health.empty() ? -1 : health_of(health.front());
    // regen.lua's once-a-second +2 may land between the last update A saw and the save, and again on the first
    // tick after the relog.
    x.check.expect(first >= before - 0.01 && first <= before + 4.01,
                   "Health persisted across logout/login: " + std::to_string(before) + " -> " + std::to_string(first));
    x.check.expect(wallet.size() == 1 && gold_of(wallet[0]) == 2, "Wallet persisted across logout/login (gold 2)");
    std::printf("COMPONENTS %d %.4f\n", wallet.empty() ? -1 : gold_of(wallet.back()),
                health.empty() ? -1.0 : health_of(health.back()));
    std::fflush(stdout);
    again->close();

    // Server-authoritative: a client's ComponentUpdate is not a valid message.
    b->send(proto::ComponentUpdate{B, "Health", R"({"current":100})"});
    const bool dropped = co_await b->wait_closed(Millis(3000));
    x.check.expect(dropped && b->disconnectReason == Code::unexpected_message,
                   "a client's ComponentUpdate gets Disconnect{unexpected_message}");
}

// After a world restart: A's Wallet and Health (from --expect-components gold,health) were persisted.
asio::awaitable<void> components_verify(Ctx& x) {
    const std::string tag = x.o.extra.count("tag") ? x.o.extra.at("tag") : "s13";
    int gold = 0;
    double health = 0;
    char comma = 0;
    std::istringstream in(x.o.extra.count("expect-components") ? x.o.extra.at("expect-components") : "");
    in >> gold >> comma >> health;
    if (!in || comma != ',') {
        x.check.expect(false, "--expect-components gold,health");
        co_return;
    }
    proto::EnterWorldOk e;
    auto c = co_await x.enter(tag + "a", e);
    if (!c) co_return;
    auto msgs = co_await drain(*c, Millis(1000));
    const auto h = updates_for(msgs, e.entityId, "Health"), w = updates_for(msgs, e.entityId, "Wallet");
    const double got = h.empty() ? -1 : health_of(h.front());
    x.check.expect(got >= health - 0.01 && got <= health + 4.01,
                   "Health survived the restart: " + std::to_string(health) + " -> " + std::to_string(got));
    x.check.expect(w.size() == 1 && gold_of(w[0]) == gold + 1,
                   "Wallet survived the restart (gold " + std::to_string(gold) + " + 1 for this login)");
    c->close();
}

// --- 14: server events, with the sample lua/systems/events.lua ---

// The ServerEvents named name whose payload's "entity" is entity.
std::vector<nlohmann::json> events_about(const std::vector<proto::Message>& msgs, const std::string& name, uint32_t entity) {
    std::vector<nlohmann::json> out;
    for (const auto& e : only<proto::ServerEvent>(msgs)) {
        auto data = nlohmann::json::parse(e.json, nullptr, false);
        if (e.name == name && data.is_object() && data.value("entity", 0u) == entity) out.push_back(std::move(data));
    }
    return out;
}

size_t count_named(const std::vector<proto::Message>& msgs, const std::string& name) {
    size_t n = 0;
    for (const auto& e : only<proto::ServerEvent>(msgs)) n += e.name == name;
    return n;
}

// A walks out of view; B and C stand at the spawn point. When C logs in: only C gets "welcome" (sendTo); C and B,
// not A, get "player.arrived" (sendNearby, after Spawn(C)). When C logs out: A and B get "player.left" (broadcast),
// B before Despawn(C).
asio::awaitable<void> events(Ctx& x) {
    proto::EnterWorldOk ea, eb, ec;
    auto a = co_await x.enter("s14a", ea);
    if (!a) co_return;
    auto own = co_await drain(*a, Millis(500));
    x.check.expect(count_named(own, "welcome") == 1, "A gets its own welcome");
    co_await walk(*a, ea.pos, 150, 0); // past the 128 m view radius of the spawn point
    auto b = co_await x.enter("s14b", eb);
    if (!b) co_return;
    co_await drain(*b, Millis(500));
    co_await drain(*a, Millis(100));
    auto c = co_await x.enter("s14c", ec);
    if (!c) co_return;
    const uint32_t C = ec.entityId;
    auto cMsgs = co_await drain(*c, Millis(1000));
    auto bMsgs = co_await drain(*b, Millis(100));
    auto aMsgs = co_await drain(*a, Millis(100));

    x.check.expect(count_named(cMsgs, "welcome") == 1 && count_named(bMsgs, "welcome") == 0 &&
                       count_named(aMsgs, "welcome") == 0,
                   "sendTo: only C gets its welcome");
    x.check.expect(events_about(cMsgs, "player.arrived", C).size() == 1, "sendNearby: C (the owner) gets player.arrived");
    const auto arrived = events_about(bMsgs, "player.arrived", C);
    x.check.expect(arrived.size() == 1 && !arrived[0].value("name", "").empty(), "sendNearby: B (in view) gets player.arrived for C");
    x.check.expect(events_about(aMsgs, "player.arrived", C).empty(), "sendNearby: A (out of view) gets nothing");
    const size_t spawnC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* sp = std::get_if<proto::Spawn>(&m);
        return sp && sp->entityId == C;
    });
    const size_t arrivedC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* e = std::get_if<proto::ServerEvent>(&m);
        return e && e->name == "player.arrived";
    });
    x.check.expect(spawnC < arrivedC && arrivedC < bMsgs.size(), "B gets Spawn(C) before player.arrived");

    c->close();
    bMsgs = co_await drain(*b, Millis(1000));
    aMsgs = co_await drain(*a, Millis(100));
    x.check.expect(events_about(aMsgs, "player.left", C).size() == 1, "broadcast: A (out of view) gets player.left");
    x.check.expect(events_about(bMsgs, "player.left", C).size() == 1, "broadcast: B gets player.left");
    const size_t leftC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* e = std::get_if<proto::ServerEvent>(&m);
        return e && e->name == "player.left";
    });
    const size_t despawnC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* d = std::get_if<proto::Despawn>(&m);
        return d && d->entityId == C;
    });
    x.check.expect(leftC < despawnC && despawnC < bMsgs.size(), "B gets player.left before Despawn(C)");

    // Server-to-client only: a client's ServerEvent is not a valid message.
    b->send(proto::ServerEvent{"cheat", "{}"});
    const bool dropped = co_await b->wait_closed(Millis(3000));
    x.check.expect(dropped && b->disconnectReason == Code::unexpected_message,
                   "a client's ServerEvent gets Disconnect{unexpected_message}");
    a->close();
}

} // namespace

int run_scenario(const Options& o) {
    asio::io_context io;
    asio::ssl::context tls = make_client_tls(o);
    Ctx x{o, tls, AuthClient(o.auth), {}};
    using Fn = asio::awaitable<void> (*)(Ctx&);
    const std::map<std::string, Fn> all = {
        {"auth", auth_handshake},       {"characters", characters},   {"ownership", ownership},
        {"aoi", aoi},                   {"chat", chat},               {"speedhack", speedhack},
        {"malformed", malformed},       {"handshake", handshake_rejects}, {"duplicate", duplicate_login},
        {"persist", persist_relogin},   {"shutdown", shutdown_phase1}, {"restart-verify", restart_verify},
        {"dbdown", db_down},            {"components", components},   {"components-verify", components_verify},
        {"events", events},
    };
    auto it = all.find(o.scenario);
    if (it == all.end()) {
        std::fprintf(stderr, "unknown scenario '%s'\n", o.scenario.c_str());
        return 2;
    }
    std::printf("scenario %s\n", o.scenario.c_str());
    asio::co_spawn(io, it->second(x), [&](std::exception_ptr e) {
        io.stop(); // clients left open by a failed step must not keep the run alive
        if (!e) return;
        try { std::rethrow_exception(e); } catch (const std::exception& ex) { x.check.expect(false, std::string("exception: ") + ex.what()); }
    });
    io.run();
    std::printf("%s %s\n", x.check.ok ? "PASS" : "FAIL", o.scenario.c_str());
    return x.check.ok ? 0 : 1;
}

} // namespace kast::bot
