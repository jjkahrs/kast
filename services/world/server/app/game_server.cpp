#include "game_server.h"
#include "../core/log.h"
#include "../world/rules.h"

namespace kast {
namespace {

world::WorldRules world_rules(const Config& c) {
    world::WorldRules r;
    r.move = {c.maxSpeed, c.speedTolerance, c.moveBurstSec, c.minY, c.maxY, c.groundTolerance, c.maxAir, c.moveNavCheck};
    r.chat.maxBytes = c.chatMaxBytes;
    r.cellSize = c.aoiCellSize;
    r.viewRadius = c.aoiViewRadius;
    r.startMap = c.startMap;
    r.chatRateCount = c.chatRateCount;
    r.chatRateWindow = Millis(c.chatRateWindowMs);
    r.saveInterval = Millis(c.saveIntervalMs);
    r.tickSeconds = 1.0f / static_cast<float>(c.tickHz);
    r.maxNpcs = c.maxNpcs;
    return r;
}

net::SessionLimits session_limits(const Config& c) {
    net::SessionLimits l;
    l.maxFrameBytes = c.maxFrameBytes;
    l.handshakeTimeout = Millis(c.handshakeTimeoutMs);
    l.idleTimeout = Millis(c.idleTimeoutMs);
    l.pingInterval = Millis(c.pingIntervalMs);
    l.msgRate = c.msgRatePerSec;
    l.msgBurst = c.msgBurst;
    l.msgRateGrace = Millis(c.msgRateGraceMs);
    l.maxSendBuffer = c.maxSendBufferBytes;
    l.simLatency = Millis(c.simLatencyMs);
    l.simJitter = Millis(c.simJitterMs);
    return l;
}

const char* role_name(proto::Role r) {
    switch (r) {
    case proto::Role::gm: return "gm";
    case proto::Role::admin: return "admin";
    default: return "player";
    }
}

} // namespace

GameServer::GameServer(asio::io_context& io, const Config& cfg, const world::Maps& maps, asio::ssl::context& tls,
                       db::DbWorker& db, TokenService& tokens, lua::Defs defs)
    : io_(io), cfg_(cfg), maps_(maps), tls_(tls), db_(db), tokens_(tokens), limits_(session_limits(cfg)), acceptor_(io),
      world_(world_rules(cfg), maps,
             [this](uint32_t sid, const proto::Message& m) {
                 if (auto* c = find(sid)) c->session->send(m);
             },
             [this](const db::CharSave& s) { db_.save(s); }, std::move(defs)),
      tickTimer_(io), tickPeriod_(Millis(1000 / cfg.tickHz)), windowTicks_(std::make_unique<TickStats>()),
      totalTicks_(std::make_unique<TickStats>()), windowLag_(std::make_unique<TickStats>()),
      totalLag_(std::make_unique<TickStats>()), shutdownTimer_(io) {}

void GameServer::start() {
    const asio::ip::tcp::endpoint ep(asio::ip::make_address(cfg_.host), cfg_.port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    log::info("listening", {{"host", cfg_.host}, {"port", cfg_.port}, {"realm", cfg_.realmId}, {"tickHz", cfg_.tickHz}});
    accept();
    nextTick_ = SteadyClock::now();
    windowStart_ = nextTick_;
    schedule_tick();
}

void GameServer::accept() {
    acceptor_.async_accept([this](asio::error_code ec, asio::ip::tcp::socket socket) {
        if (shuttingDown_ || !acceptor_.is_open()) return;
        if (!ec) {
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
            const uint32_t id = nextSessionId_++;
            auto session = std::make_shared<net::Session>(
                id, std::move(socket), tls_, limits_,
                [this](net::Session& s, proto::Message&& m) { on_message(s, std::move(m)); },
                [this](net::Session& s) { on_closed(s); });
            if (clients_.size() >= cfg_.maxConnections) {
                session->start(proto::Code::server_full);
            } else {
                clients_[id].session = session;
                session->start();
            }
        }
        accept();
    });
}

GameServer::Client* GameServer::find(uint32_t sessionId) {
    auto it = clients_.find(sessionId);
    return it == clients_.end() ? nullptr : &it->second;
}

void GameServer::on_message(net::Session& s, proto::Message&& m) {
    Client* c = find(s.id());
    if (!c || shuttingDown_) return;
    switch (c->phase) {
    case Phase::hello:
        if (auto* h = std::get_if<proto::Hello>(&m)) handle_hello(*c, *h);
        else s.disconnect(proto::Code::unexpected_message);
        break;
    case Phase::auth:
        if (auto* a = std::get_if<proto::Auth>(&m)) handle_auth(*c, *a);
        else s.disconnect(proto::Code::unexpected_message);
        break;
    case Phase::verifying: s.disconnect(proto::Code::unexpected_message); break;
    case Phase::lobby: handle_lobby(*c, std::move(m)); break;
    case Phase::world: handle_world(*c, std::move(m)); break;
    }
}

void GameServer::handle_hello(Client& c, const proto::Hello& m) {
    if (m.protocolVersion != proto::kProtocolVersion) {
        c.session->disconnect(proto::Code::protocol_mismatch);
        return;
    }
    c.phase = Phase::auth;
}

void GameServer::handle_auth(Client& c, const proto::Auth& m) {
    c.phase = Phase::verifying;
    const uint32_t sid = c.session->id();
    tokens_.verify(m.accessToken, [this, sid](TokenResult r) { auth_done(sid, r); }); // never log the token
}

void GameServer::auth_done(uint32_t sid, const TokenResult& r) {
    Client* c = find(sid);
    if (!c || c->phase != Phase::verifying || shuttingDown_) return;
    if (r.code != proto::Code::ok) {
        log::info("auth rejected", {{"session", sid}, {"reason", proto::code_name(r.code)}});
        c->session->disconnect(r.code);
        return;
    }
    if (auto it = online_.find(r.accountId); it != online_.end() && it->second != sid) {
        if (Client* old = find(it->second)) {
            log::info("duplicate login: kicking old session", {{"account", r.accountId}, {"old", it->second}, {"new", sid}});
            leave_world(*old); // its save is queued before anything the new session can ask for
            old->session->disconnect(proto::Code::logged_in_elsewhere);
        }
    }
    online_[r.accountId] = sid;
    c->accountId = r.accountId;
    c->role = r.role;
    c->phase = Phase::lobby;
    c->session->authenticated();
    c->session->send(proto::AuthOk{r.accountId, r.role});
    log::info("authenticated", {{"session", sid}, {"account", r.accountId}, {"role", role_name(r.role)}});
}

void GameServer::char_error(Client& c, proto::Opcode request, proto::Code code) {
    c.session->send(proto::CharError{request, code});
}

void GameServer::handle_lobby(Client& c, proto::Message&& m) {
    const proto::Opcode op = proto::opcode_of(m);
    const bool charRequest = op == proto::Opcode::CharListReq || op == proto::Opcode::CharCreate ||
                             op == proto::Opcode::CharDelete || op == proto::Opcode::EnterWorld;
    if (!charRequest) {
        if (op == proto::Opcode::Move || op == proto::Opcode::Say || op == proto::Opcode::ClientEvent ||
            op == proto::Opcode::Interact)
            return; // stale in-world traffic; harmless
        c.session->disconnect(proto::Code::unexpected_message);
        return;
    }
    if (c.requestInFlight) { char_error(c, op, proto::Code::rate_limited); return; }

    const uint32_t sid = c.session->id();
    const uint64_t acc = c.accountId;
    // Runs on the game thread when the DB reply arrives; the client may have left meanwhile.
    auto reply = [this, sid]() -> Client* {
        Client* cl = find(sid);
        if (!cl || cl->phase != Phase::lobby) return nullptr;
        cl->requestInFlight = false;
        return cl;
    };

    if (std::holds_alternative<proto::CharListReq>(m)) {
        c.requestInFlight = true;
        db_.query<std::vector<db::CharRow>>(
            [acc](db::CharRepo& repo) { return repo.list(acc); },
            [this, reply](std::optional<std::vector<db::CharRow>> rows) {
                Client* cl = reply();
                if (!cl) return;
                if (!rows) return char_error(*cl, proto::Opcode::CharListReq, proto::Code::service_unavailable);
                proto::CharList list;
                for (const auto& r : *rows) list.chars.push_back({r.id, r.name, r.pos, r.options});
                cl->session->send(list);
            });
    } else if (auto* create = std::get_if<proto::CharCreate>(&m)) {
        if (!valid_character_name(create->name)) return char_error(c, op, proto::Code::name_invalid);
        // C++ limits first: the Lua hook only ever sees small, plain options.
        if (!proto::valid_char_options(create->options) || !world_.validate_character(create->name, create->options))
            return char_error(c, op, proto::Code::options_invalid);
        c.requestInFlight = true;
        const auto& start = maps_.info(cfg_.startMap);
        const uint16_t map = start.id;
        const proto::Vec3 spawn{start.spawn[0], start.spawn[1], start.spawn[2]};
        const float heading = start.spawnHeading;
        const uint32_t maxChars = cfg_.maxCharsPerAccount;
        db_.query<db::CreateResult>(
            [acc, name = create->name, options = create->options, map, spawn, heading, maxChars](db::CharRepo& repo) {
                return repo.create(acc, name, options, map, spawn, heading, maxChars);
            },
            [this, reply](std::optional<db::CreateResult> res) {
                Client* cl = reply();
                if (!cl) return;
                if (!res) return char_error(*cl, proto::Opcode::CharCreate, proto::Code::service_unavailable);
                if (res->code != proto::Code::ok) return char_error(*cl, proto::Opcode::CharCreate, res->code);
                cl->session->send(proto::CharCreated{{res->row.id, res->row.name, res->row.pos, res->row.options}});
                log::info("character created", {{"account", cl->accountId}, {"char", res->row.id}});
            });
    } else if (auto* del = std::get_if<proto::CharDelete>(&m)) {
        c.requestInFlight = true;
        const uint64_t charId = del->charId;
        db_.query<bool>(
            [acc, charId](db::CharRepo& repo) { return repo.soft_delete(acc, charId); },
            [this, reply, charId](std::optional<bool> ok) {
                Client* cl = reply();
                if (!cl) return;
                if (!ok) return char_error(*cl, proto::Opcode::CharDelete, proto::Code::service_unavailable);
                if (!*ok) return char_error(*cl, proto::Opcode::CharDelete, proto::Code::not_found);
                cl->session->send(proto::CharDeleted{charId});
                log::info("character deleted", {{"account", cl->accountId}, {"char", charId}});
            });
    } else if (auto* enter = std::get_if<proto::EnterWorld>(&m)) {
        c.requestInFlight = true;
        const uint64_t charId = enter->charId;
        db_.query_for_char<std::optional<db::CharRow>>(
            charId, [acc, charId](db::CharRepo& repo) { return repo.enter(acc, charId); },
            [this, reply, sid](std::optional<std::optional<db::CharRow>> row) {
                Client* cl = reply();
                if (!cl) return;
                if (!row) return char_error(*cl, proto::Opcode::EnterWorld, proto::Code::service_unavailable);
                if (!*row) return char_error(*cl, proto::Opcode::EnterWorld, proto::Code::not_found);
                cl->entity = world_.spawn(sid, **row, SteadyClock::now());
                cl->phase = Phase::world;
                log::info("entered world", {{"account", cl->accountId}, {"char", (*row)->id},
                                            {"population", world_.population()}});
            });
    }
}

void GameServer::handle_world(Client& c, proto::Message&& m) {
    if (auto* mv = std::get_if<proto::Move>(&m)) { world_.push_move(c.entity, *mv); return; }
    if (auto* say = std::get_if<proto::Say>(&m)) { world_.push_say(c.entity, std::move(say->text)); return; }
    if (auto* in = std::get_if<proto::Interact>(&m)) { world_.push_interact(c.entity, in->objectId); return; }
    if (auto* ev = std::get_if<proto::ClientEvent>(&m)) {
        auto data = client_event_data(*ev);
        if (!data) {
            log::info("client event rejected", {{"session", c.session->id()}, {"bytes", ev->json.size()}});
            c.session->disconnect(proto::Code::malformed);
            return;
        }
        world_.push_client_event(c.entity, std::move(ev->name), std::move(*data), ev->requestId);
        return;
    }
    const proto::Opcode op = proto::opcode_of(m);
    if (op == proto::Opcode::CharListReq || op == proto::Opcode::CharCreate || op == proto::Opcode::CharDelete ||
        op == proto::Opcode::EnterWorld) {
        char_error(c, op, proto::Code::already_in_world);
        return;
    }
    c.session->disconnect(proto::Code::unexpected_message);
}

void GameServer::leave_world(Client& c) {
    if (c.phase != Phase::world) return;
    world_.despawn(c.entity); // queues the save
    c.entity = entt::null;
    c.phase = Phase::lobby;
}

void GameServer::on_closed(net::Session& s) {
    auto it = clients_.find(s.id());
    if (it == clients_.end()) return;
    Client& c = it->second;
    leave_world(c);
    if (auto o = online_.find(c.accountId); o != online_.end() && o->second == s.id()) online_.erase(o);
    clients_.erase(it);
    if (shuttingDown_ && clients_.empty() && onShutdownDone_) {
        shutdownTimer_.cancel();
        auto done = std::move(onShutdownDone_);
        onShutdownDone_ = nullptr;
        done();
    }
}

// --- tick ---

void GameServer::schedule_tick() {
    nextTick_ += tickPeriod_;
    tickTimer_.expires_at(nextTick_);
    tickTimer_.async_wait([this](asio::error_code ec) {
        if (!ec && !shuttingDown_) tick();
    });
}

void GameServer::tick() {
    const TimePoint start = SteadyClock::now();
    const double lagMs = std::chrono::duration<double, std::milli>(start - nextTick_).count();
    world_.tick(start);
    const double ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
    windowTicks_->add(ms);
    totalTicks_->add(ms);
    windowLag_->add(lagMs);
    totalLag_->add(lagMs);
    if (start - windowStart_ >= std::chrono::seconds(cfg_.tickStatsIntervalSec)) {
        log_tick_stats("tick stats", *windowTicks_, *windowLag_);
        windowTicks_->reset();
        windowLag_->reset();
        windowStart_ = start;
    }
    if (SteadyClock::now() > nextTick_ + tickPeriod_) nextTick_ = SteadyClock::now(); // fell behind: don't burst
    schedule_tick();
}

void GameServer::log_tick_stats(const char* msg, const TickStats& ticks, const TickStats& lag) {
    const auto saves = db_.stats();
    log::info(msg, {{"ticks", ticks.count()},
                    {"p50_ms", ticks.percentile(0.50)},
                    {"p99_ms", ticks.percentile(0.99)},
                    {"max_ms", ticks.max()},
                    {"lag_p99_ms", lag.percentile(0.99)},
                    {"lag_max_ms", lag.max()},
                    {"population", world_.population()},
                    {"connections", clients_.size()},
                    {"saves_ok", saves.ok},
                    {"saves_failed_attempts", saves.failedAttempts},
                    {"saves_pending", saves.pending}});
}

// --- shutdown ---

void GameServer::shutdown(std::function<void()> onDone) {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    log::info("shutting down", {{"connections", clients_.size()}, {"population", world_.population()}});
    asio::error_code ec;
    acceptor_.close(ec);
    tickTimer_.cancel();
    log_tick_stats("tick stats total", *totalTicks_, *totalLag_);

    world_.shutdown_scripts(); // onLeave changes are part of the saves below
    onShutdownDone_ = std::move(onDone);
    std::vector<std::shared_ptr<net::Session>> sessions;
    for (auto& [id, c] : clients_) {
        leave_world(c);
        sessions.push_back(c.session);
    }
    world_.remove_npcs(); // no players left to tell; NPCs are never saved
    for (auto& s : sessions) s->disconnect(proto::Code::server_shutdown);
    if (clients_.empty()) {
        auto done = std::move(onShutdownDone_);
        onShutdownDone_ = nullptr;
        done();
        return;
    }
    // Clients that never read get closed by their session's 2 s grace timer; this is a backstop.
    shutdownTimer_.expires_after(Millis(3000));
    shutdownTimer_.async_wait([this](asio::error_code e) {
        if (e || !onShutdownDone_) return;
        auto done = std::move(onShutdownDone_);
        onShutdownDone_ = nullptr;
        done();
    });
}

} // namespace kast
