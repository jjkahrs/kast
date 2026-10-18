// kast_bot: headless test client for the world server.
//   kast_bot --scenario <name> [options]                       one integration scenario, prints PASS/FAIL
//   kast_bot --mode load --count 500 --behaviour wander [...]  many sessions in one process
#include "load.h"
#include "scenarios.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace kast::bot;

namespace {

void usage() {
    std::printf(
        "usage: kast_bot [--scenario NAME | --mode load] [options]\n"
        "  --auth-url URL        auth service (default http://localhost:3100)\n"
        "  --mailpit-url URL     Mailpit API (default http://localhost:8026)\n"
        "  --host HOST --port N  world server (default localhost 7777)\n"
        "  --ca FILE             certificate/CA the server must present\n"
        "  --run-id ID           namespaces bot accounts (default run)\n"
        "  --count N             load: bot sessions (default 1)\n"
        "  --behaviour B         load: idle | wander | chat | speedhack | malformed (default wander)\n"
        "  --duration S          load: seconds after the last bot connected (default 60)\n"
        "  --threads N           load: io threads (default 4)\n"
        "  --connect-rate N      load: new sessions per second (default 50)\n"
        "  --auth-key FILE       handshake scenario: auth's private key, to mint an expired token (test only)\n"
        "  --expect-pos X,Y,Z    restart-verify scenario\n"
        "  --map-root DIR        terrain/navmesh scenarios: the server's maps/ folder (ground heights)\n"
        "  --expect-components G,H  components-verify scenario: Wallet gold and Health current before the restart\n"
        "  --tag T               shutdown/restart-verify scenarios: account tag\n"
        "scenarios: auth characters ownership aoi chat speedhack malformed handshake duplicate persist\n"
        "           shutdown restart-verify dbdown components components-verify events fsm npcs\n"
        "           portal respawn terrain navmesh client-events client-event-rate\n"
        "           combat-bat combat-rules (these two need the starter-mmo override)\n");
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--auth-url") o.auth.authUrl = val();
        else if (a == "--mailpit-url") o.auth.mailpitUrl = val();
        else if (a == "--host") o.host = val();
        else if (a == "--port") o.port = static_cast<uint16_t>(std::stoi(val()));
        else if (a == "--ca") o.caFile = val();
        else if (a == "--run-id") o.runId = val();
        else if (a == "--mode") o.mode = val();
        else if (a == "--scenario") o.scenario = val();
        else if (a == "--behaviour") o.behaviour = val();
        else if (a == "--count") o.count = std::stoi(val());
        else if (a == "--duration") o.durationSec = std::stoi(val());
        else if (a == "--threads") o.threads = std::stoi(val());
        else if (a == "--connect-rate") o.connectRatePerSec = std::stoi(val());
        else if (a == "--auth-key") o.authKeyFile = val();
        else if (a == "--expect-pos") o.expectPos = val();
        else if (a == "--map-root") o.mapRoot = val();
        else if (a == "--tag") o.extra["tag"] = val();
        else if (a == "--expect-components") o.extra["expect-components"] = val();
        else if (a == "--zone") o.extra["zone"] = val();
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    try {
        if (o.mode == "load") return run_load(o);
        if (o.scenario.empty()) { usage(); return 2; }
        return run_scenario(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
