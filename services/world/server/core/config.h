// All settings come from environment variables, validated at startup (see README.md for the table).
#pragma once
#include "log.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kast {

struct Config {
    // network
    std::string host = "0.0.0.0";
    uint16_t port = 7777;
    uint32_t maxConnections = 1000;
    uint32_t maxFrameBytes = 4096;
    uint32_t handshakeTimeoutMs = 10000;
    uint32_t idleTimeoutMs = 60000;
    uint32_t pingIntervalMs = 20000;
    double msgRatePerSec = 60;
    double msgBurst = 120;
    uint32_t msgRateGraceMs = 2000;
    uint32_t maxSendBufferBytes = 1 << 20;
    // TLS
    std::string tlsCertFile, tlsKeyFile;
    std::string tlsDevCertDir = "./certs";
    // auth
    std::string jwksUrl;
    std::string jwtIssuer = "kast-auth";
    std::string jwtAudience = "kast";
    uint32_t jwtClockSkewSec = 5;
    uint32_t jwksRefreshSec = 300;
    uint32_t jwksMinRefetchSec = 10;
    uint32_t jwksTimeoutMs = 3000;
    // database
    std::string dbHost;
    uint16_t dbPort = 3306;
    std::string dbUser, dbPassword, dbName;
    uint32_t dbConnectTimeoutSec = 3;
    std::string migrationsDir = "./migrations";
    // world
    uint32_t realmId = 0;
    uint32_t tickHz = 20;
    float zoneSizeX = 2048, zoneSizeZ = 2048;
    std::optional<float> spawnX, spawnZ; // default: zone centre
    float spawnY = 0;
    float minY = -100, maxY = 500;
    float maxSpeed = 10, speedTolerance = 1, moveBurstSec = 1;
    float aoiCellSize = 64, aoiViewRadius = 128;
    uint32_t maxCharsPerAccount = 10;
    uint32_t chatMaxBytes = 256;
    uint32_t chatRateCount = 5;
    uint32_t chatRateWindowMs = 10000;
    // persistence / ops
    uint32_t saveIntervalMs = 60000;
    uint32_t saveRetryMinMs = 500;
    uint32_t saveRetryMaxMs = 30000;
    uint32_t shutdownTimeoutMs = 10000;
    uint32_t tickStatsIntervalSec = 10;
    std::string luaRoot = "/app/lua"; // components/ and systems/
    log::Level logLevel = log::Level::info;

    float spawn_x() const { return spawnX.value_or(zoneSizeX / 2); }
    float spawn_z() const { return spawnZ.value_or(zoneSizeZ / 2); }
};

using EnvLookup = std::function<std::optional<std::string>(const char* name)>;

struct ConfigResult {
    Config config;
    std::vector<std::string> errors; // one line per bad or missing variable
};

ConfigResult load_config(const EnvLookup& env);
std::optional<std::string> process_env(const char* name);

} // namespace kast
