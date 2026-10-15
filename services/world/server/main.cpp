// Kast World Server: TLS game connections, auth-service JWTs, one zone on a fixed tick, MySQL persistence.
// Threads: io (network + tick), DB worker, token verifier.
#include "app/game_server.h"
#include "auth/token_service.h"
#include "core/config.h"
#include "core/log.h"
#include "db/db_worker.h"
#include "db/migrate.h"
#include "net/tls_context.h"

#include <asio.hpp>

#include <cstdio>

using namespace kast;

int main() {
    const ConfigResult loaded = load_config(process_env);
    if (!loaded.errors.empty()) {
        std::fprintf(stderr, "Invalid configuration:\n");
        for (const auto& e : loaded.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }
    const Config& cfg = loaded.config;
    log::set_level(cfg.logLevel);

    db::library_init();
    const db::ConnectOptions dbOpts{cfg.dbHost, cfg.dbPort, cfg.dbUser, cfg.dbPassword, cfg.dbName,
                                    cfg.dbConnectTimeoutSec, 5};
    try {
        db::Connection conn(dbOpts);
        const auto applied = db::migrate(conn, cfg.migrationsDir);
        log::info("migrations done", {{"applied", applied.size()}});
    } catch (const std::exception& e) {
        log::error("startup failed: database/migrations", {{"error", e.what()}});
        return 1;
    }

    try {
        asio::io_context io(1);
        asio::ssl::context tls = net::make_server_tls({cfg.tlsCertFile, cfg.tlsKeyFile, cfg.tlsDevCertDir});
        auto post = [&io](std::function<void()> f) { asio::post(io, std::move(f)); };

        db::DbWorker dbWorker({dbOpts, cfg.realmId, Millis(cfg.saveRetryMinMs), Millis(cfg.saveRetryMaxMs)}, post);
        TokenService tokens({cfg.jwksUrl, {cfg.jwtIssuer, cfg.jwtAudience, cfg.jwtClockSkewSec},
                             Millis(cfg.jwksRefreshSec * 1000ull), Millis(cfg.jwksMinRefetchSec * 1000ull),
                             Millis(cfg.jwksTimeoutMs)},
                            post);
        GameServer server(io, cfg, tls, dbWorker, tokens);
        server.start();

        TimePoint shutdownStart{};
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](asio::error_code ec, int sig) {
            if (ec) return;
            log::info("signal received", {{"signal", sig}});
            shutdownStart = SteadyClock::now();
            server.shutdown([&io] { io.stop(); });
        });
        io.run();

        // Everything after the last client is gone: flush saves within what is left of the shutdown budget.
        tokens.stop();
        const auto deadline = shutdownStart + Millis(cfg.shutdownTimeoutMs) - Millis(500);
        const size_t lost = dbWorker.stop(deadline);
        const auto stats = dbWorker.stats();
        if (lost > 0) {
            log::error("shutdown: saves not flushed", {{"pending", lost}});
            return 1;
        }
        log::info("shutdown complete", {{"saves_ok", stats.ok}, {"saves_failed_attempts", stats.failedAttempts}});
        return 0;
    } catch (const std::exception& e) {
        log::error("fatal", {{"error", e.what()}});
        return 1;
    }
}
