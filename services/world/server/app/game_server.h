// Glues the pieces together on the io thread: accepts sessions, walks each through
// Hello -> Auth -> lobby (character requests) -> world, runs the fixed-rate tick and the graceful shutdown.
#pragma once
#include "../auth/token_service.h"
#include "../core/config.h"
#include "../core/tick_stats.h"
#include "../db/db_worker.h"
#include "../net/session.h"
#include "../world/world.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <functional>
#include <memory>
#include <unordered_map>

namespace kast {

class GameServer {
public:
    // maps must outlive the server and hold cfg.startMap.
    GameServer(asio::io_context& io, const Config& cfg, const world::Maps& maps, asio::ssl::context& tls,
               db::DbWorker& db, TokenService& tokens, lua::Defs defs);

    // Loads the Lua systems under LUA_ROOT and runs their init; every error (empty: OK to start).
    std::vector<std::string> load_scripts() { return world_.load_scripts(cfg_.luaRoot, cfg_.gameStateMachines); }
    std::vector<lua::SystemStatus> script_status() const { return world_.script_status(); }
    void start();
    // Stops accepting, saves and disconnects everyone with server_shutdown, then calls onDone.
    void shutdown(std::function<void()> onDone);

private:
    enum class Phase { hello, auth, verifying, lobby, world };
    struct Client {
        std::shared_ptr<net::Session> session;
        Phase phase = Phase::hello;
        uint64_t accountId = 0;
        proto::Role role = proto::Role::player;
        bool requestInFlight = false; // one character request at a time per client
        entt::entity entity = entt::null;
    };

    void accept();
    void on_message(net::Session& s, proto::Message&& m);
    void on_closed(net::Session& s);
    Client* find(uint32_t sessionId);

    void handle_hello(Client& c, const proto::Hello& m);
    void handle_auth(Client& c, const proto::Auth& m);
    void auth_done(uint32_t sessionId, const TokenResult& r);
    void handle_lobby(Client& c, proto::Message&& m);
    void handle_world(Client& c, proto::Message&& m);
    void char_error(Client& c, proto::Opcode request, proto::Code code);
    void leave_world(Client& c);

    void schedule_tick();
    void tick();
    void log_tick_stats(const char* msg, const TickStats& ticks, const TickStats& lag);

    asio::io_context& io_;
    const Config& cfg_;
    const world::Maps& maps_;
    asio::ssl::context& tls_;
    db::DbWorker& db_;
    TokenService& tokens_;
    net::SessionLimits limits_;
    asio::ip::tcp::acceptor acceptor_;
    world::World world_;

    uint32_t nextSessionId_ = 1;
    std::unordered_map<uint32_t, Client> clients_;
    std::unordered_map<uint64_t, uint32_t> online_; // accountId -> session id

    asio::steady_timer tickTimer_;
    TimePoint nextTick_{};
    Millis tickPeriod_;
    // Heap: each histogram is ~400 KB.
    std::unique_ptr<TickStats> windowTicks_, totalTicks_, windowLag_, totalLag_;
    TimePoint windowStart_{};

    bool shuttingDown_ = false;
    std::function<void()> onShutdownDone_;
    asio::steady_timer shutdownTimer_;
};

} // namespace kast
