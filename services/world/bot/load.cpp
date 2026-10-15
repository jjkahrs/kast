#include "load.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <random>
#include <thread>

namespace kast::bot {
namespace {

struct Stats {
    std::atomic<int> tokens{0}, connected{0}, entered{0}, unexpectedDisconnects{0}, expectedDisconnects{0}, errors{0};
    std::atomic<uint64_t> movesSent{0}, entityMoves{0}, spawns{0}, despawns{0}, corrections{0}, chats{0}, says{0};
    std::mutex mu;
    std::vector<std::string> errorSamples;

    void error(const std::string& e) {
        ++errors;
        std::lock_guard lk(mu);
        if (errorSamples.size() < 10) errorSamples.push_back(e);
    }
};

// One bot's random walk: straight segments of 3-8 s at 9 m/s, bouncing off the zone edge.
struct Wander {
    std::mt19937 rng{std::random_device{}()};
    float heading = 0;
    int stepsLeft = 0;
    proto::Vec3 step(proto::Vec3 p, float zone) {
        if (stepsLeft-- <= 0) {
            heading = std::uniform_real_distribution<float>(0, 6.2831853f)(rng);
            stepsLeft = std::uniform_int_distribution<int>(30, 80)(rng);
        }
        const float d = 0.9f; // 9 m/s at 10 Hz
        proto::Vec3 n{p.x + d * std::sin(heading), p.y, p.z + d * std::cos(heading)};
        if (n.x < 5 || n.x > zone - 5 || n.z < 5 || n.z > zone - 5) {
            heading += 3.14159265f;
            n = {p.x + d * std::sin(heading), p.y, p.z + d * std::cos(heading)};
        }
        return n;
    }
};

asio::awaitable<void> run_bot(const Options& o, asio::ssl::context& tls, Stats& st, std::string token,
                              Millis startDelay, std::chrono::steady_clock::time_point end) {
    co_await sleep_for(startDelay);
    std::string err;
    auto c = co_await connect_and_login(o, tls, token, err);
    if (!c) { st.error(err); co_return; }
    ++st.connected;

    if (o.behaviour == "malformed") {
        c->send_raw({0, 0, 0x77, 0x77});
        const bool closed = co_await c->wait_closed(Millis(5000));
        if (closed && c->disconnectReason == proto::Code::unknown_opcode) ++st.expectedDisconnects;
        else st.error("malformed bot was not disconnected with unknown_opcode");
        co_return;
    }

    proto::EnterWorldOk ok;
    if (!co_await enter_world(*c, random_name(), ok, err)) {
        st.error("enter: " + err);
        c->close();
        co_return;
    }
    ++st.entered;

    auto pos = std::make_shared<proto::Vec3>(ok.pos);
    c->onMessage = [&st, pos](const proto::Message& m) {
        if (std::holds_alternative<proto::EntityMove>(m)) ++st.entityMoves;
        else if (std::holds_alternative<proto::Spawn>(m)) ++st.spawns;
        else if (std::holds_alternative<proto::Despawn>(m)) ++st.despawns;
        else if (std::holds_alternative<proto::ChatMsg>(m)) ++st.chats;
        else if (auto* corr = std::get_if<proto::Correction>(&m)) {
            ++st.corrections;
            *pos = corr->pos;
        }
    };

    Wander wander;
    const float zone = o.extra.count("zone") ? std::stof(o.extra.at("zone")) : 2048.f;
    asio::steady_timer t(co_await asio::this_coro::executor);
    auto next = std::chrono::steady_clock::now();
    uint32_t tick = 0;
    while (std::chrono::steady_clock::now() < end && !c->closed()) {
        next += Millis(100);
        // A stalled bot skips steps instead of bursting them: a burst would look like a speedhack.
        if (std::chrono::steady_clock::now() > next + Millis(300)) next = std::chrono::steady_clock::now();
        t.expires_at(next);
        co_await t.async_wait(asio::use_awaitable);
        ++tick;
        if (o.behaviour == "wander" || o.behaviour == "chat") {
            *pos = wander.step(*pos, zone);
            c->send(proto::Move{*pos, wander.heading, tick * 100});
            ++st.movesSent;
        } else if (o.behaviour == "speedhack") {
            pos->x = std::min(pos->x + 5.f, zone - 1); // 50 m/s
            c->send(proto::Move{*pos, 0, tick * 100});
            ++st.movesSent;
        }
        if (o.behaviour == "chat" && tick % 40 == 0) { // every 4 s: under the 5 per 10 s limit
            c->send(proto::Say{"hello from a load bot"});
            ++st.says;
        }
    }
    if (c->closed()) {
        ++st.unexpectedDisconnects;
        st.error(std::string("disconnected: ") + (c->disconnectReason ? proto::code_name(*c->disconnectReason) : "closed"));
    }
    c->close();
}

} // namespace

int run_load(const Options& o) {
    Stats st;
    // 1. Real tokens from auth, in parallel (register + Mailpit verify on first run, then just login).
    std::vector<std::string> tokens(o.count);
    {
        std::atomic<int> nextIdx{0};
        std::vector<std::thread> pool;
        for (int w = 0; w < std::min(16, o.count); ++w)
            pool.emplace_back([&] {
                AuthClient auth(o.auth);
                for (int i = nextIdx++; i < o.count; i = nextIdx++) {
                    std::string err;
                    if (auto t = auth.ensure_login(bot_account(o, "load-" + std::to_string(i)), err)) {
                        tokens[i] = *t;
                        ++st.tokens;
                    } else {
                        st.error(err);
                    }
                }
            });
        for (auto& th : pool) th.join();
    }
    std::printf("{\"phase\":\"tokens\",\"ok\":%d,\"of\":%d}\n", st.tokens.load(), o.count);
    std::fflush(stdout);

    // 2. Sessions spread over a few io threads, connecting at connectRatePerSec.
    const auto start = std::chrono::steady_clock::now();
    const auto rampMs = static_cast<int64_t>(1000.0 * o.count / std::max(1, o.connectRatePerSec));
    const auto end = start + Millis(rampMs) + std::chrono::seconds(o.durationSec);
    std::vector<std::unique_ptr<asio::io_context>> ios;
    for (int i = 0; i < o.threads; ++i) ios.push_back(std::make_unique<asio::io_context>(1));
    asio::ssl::context tls = make_client_tls(o);
    for (int i = 0; i < o.count; ++i) {
        if (tokens[i].empty()) continue;
        const Millis delay(1000LL * i / std::max(1, o.connectRatePerSec));
        asio::co_spawn(*ios[i % o.threads], run_bot(o, tls, st, tokens[i], delay, end), [&st](std::exception_ptr e) {
            if (!e) return;
            try { std::rethrow_exception(e); } catch (const std::exception& ex) { st.error(ex.what()); }
        });
    }
    std::vector<std::thread> threads;
    for (auto& io : ios) threads.emplace_back([&io] { io->run(); });

    // progress line every 30 s
    std::thread progress([&] {
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            std::printf("{\"phase\":\"running\",\"entered\":%d,\"unexpected_disconnects\":%d,\"errors\":%d,"
                        "\"moves_sent\":%llu,\"entity_moves_recv\":%llu}\n",
                        st.entered.load(), st.unexpectedDisconnects.load(), st.errors.load(),
                        static_cast<unsigned long long>(st.movesSent.load()),
                        static_cast<unsigned long long>(st.entityMoves.load()));
            std::fflush(stdout);
        }
    });
    for (auto& th : threads) th.join();
    progress.join();

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    nlohmann::json summary = {{"phase", "summary"},
                              {"behaviour", o.behaviour},
                              {"bots", o.count},
                              {"seconds", std::round(secs)},
                              {"tokens", st.tokens.load()},
                              {"connected", st.connected.load()},
                              {"entered", st.entered.load()},
                              {"unexpected_disconnects", st.unexpectedDisconnects.load()},
                              {"expected_disconnects", st.expectedDisconnects.load()},
                              {"errors", st.errors.load()},
                              {"moves_sent", st.movesSent.load()},
                              {"entity_moves_recv", st.entityMoves.load()},
                              {"spawns_recv", st.spawns.load()},
                              {"despawns_recv", st.despawns.load()},
                              {"corrections_recv", st.corrections.load()},
                              {"says_sent", st.says.load()},
                              {"chat_recv", st.chats.load()},
                              {"error_samples", st.errorSamples}};
    std::printf("%s\n", summary.dump().c_str());

    bool ok = st.errors == 0 && st.unexpectedDisconnects == 0 && st.connected == o.count;
    if (o.behaviour == "malformed") ok = ok && st.expectedDisconnects == o.count;
    else ok = ok && st.entered == o.count;
    if (o.behaviour == "speedhack") ok = ok && st.corrections > 0;
    if (o.behaviour == "wander") ok = ok && st.corrections == 0; // honest movers are never corrected
    std::printf("%s load %s x%d\n", ok ? "PASS" : "FAIL", o.behaviour.c_str(), o.count);
    return ok ? 0 : 1;
}

} // namespace kast::bot
