#!/usr/bin/env bash
# Runs every integration scenario against the compose stack (docker compose up -d --build first).
# Prints PASS/FAIL per scenario and exits non-zero if any failed.
# Scenarios 10-13 stop/restart containers, so run this against a test stack only.
# --starter-mmo runs only scenarios 23-24 (samples/starter-mmo combat), against the stack with the sample's override.
set -u
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1 # Git Bash on Windows: keep container paths as written

RUN="${RUN_ID:-it$(date +%s)}"
LOGDIR="${LOGDIR:-build/integration-$RUN}"
mkdir -p "$LOGDIR"
WORLD=$(docker compose ps -q world)
declare -a RESULTS=()
FAILED=0

bot() { docker compose run --rm -T bot --run-id "$RUN" "$@"; }

record() { # name, ok(0/1), detail
    if [ "$2" -eq 0 ]; then RESULTS+=("PASS  $1"); else RESULTS+=("FAIL  $1  $3"); FAILED=1; fi
    echo "==> $( [ "$2" -eq 0 ] && echo PASS || echo FAIL ) $1"
}

scenario() { # name, bot args...
    local name=$1; shift
    local log="$LOGDIR/${name%% *}.log"
    echo "--- $name"
    bot "$@" 2>&1 | tee "$log" | grep -v -E '^ *(Container|Network) '
    record "$name" "${PIPESTATUS[0]}" "see $log"
}

wait_line() { # file, pattern, timeout seconds
    local i
    for ((i = 0; i < $3 * 2; i++)); do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

wait_healthy() { # service
    local i id
    for ((i = 0; i < 120; i++)); do
        id=$(docker compose ps -q "$1")
        [ -n "$id" ] && [ "$(docker inspect -f '{{.State.Health.Status}}' "$id" 2>/dev/null)" = healthy ] && return 0
        sleep 1
    done
    return 1
}

now_ms() { date +%s%3N; }

# Graceful stop of the world container while a bot is in the world; checks exit code, time and the client's
# Disconnect, then restarts and verifies the saved position. $1 = label, $2 = account tag, $3 = how (stop|restart)
graceful() {
    local name=$1 tag=$2 how=$3 log="$LOGDIR/${1%% *}.log" ok=0 detail=""
    echo "--- $name"
    bot --scenario shutdown --tag "$tag" >"$log" 2>&1 &
    local botpid=$!
    if ! wait_line "$log" '^READY' 90; then
        cat "$log"; record "$name" 1 "bot never got ready"; wait $botpid; return
    fi
    read -r _ x y z < <(grep '^LASTPOS' "$log")
    local since; since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    local t0; t0=$(now_ms)
    if [ "$how" = restart ]; then docker compose restart -t 15 world >/dev/null 2>&1; else docker compose stop -t 15 world >/dev/null 2>&1; fi
    local elapsed=$(( $(now_ms) - t0 ))
    wait $botpid; local botrc=$?
    grep -v -E '^ *(Container|Network) ' "$log"
    local logs; logs=$(docker logs --since "$since" "$WORLD" 2>&1)
    if [ "$how" = stop ]; then
        local code; code=$(docker inspect -f '{{.State.ExitCode}}' "$WORLD")
        echo "  world exit code $code after ${elapsed} ms"
        [ "$code" = 0 ] || { ok=1; detail+="exit code $code; "; }
        [ "$elapsed" -lt 10000 ] || { ok=1; detail+="took ${elapsed} ms; "; }
        docker compose start world >/dev/null 2>&1
    else
        echo "  restart took ${elapsed} ms"
    fi
    echo "$logs" | grep -E '"shutting down"|"shutdown complete"|"tick stats total"' | sed 's/^/  log: /'
    echo "$logs" | grep -q '"shutdown complete"' || { ok=1; detail+="no 'shutdown complete' log; "; }
    [ $botrc -eq 0 ] || { ok=1; detail+="client did not get server_shutdown; "; }
    wait_healthy world || { ok=1; detail+="world not healthy after restart; "; }
    bot --scenario restart-verify --tag "$tag" --expect-pos "$x,$y,$z" 2>&1 | tee -a "$log" | grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || { ok=1; detail+="position not persisted; "; }
    record "$name" $ok "$detail"
}

echo "run id: $RUN   logs: $LOGDIR"
wait_healthy world || { echo "world is not healthy; run docker compose up -d --build"; exit 1; }

summary() {
    echo
    echo "================ integration summary ($RUN) ================"
    printf '%s\n' "${RESULTS[@]}"
    exit $FAILED
}

# --starter-mmo: only the samples/starter-mmo combat scenarios, against the stack started with the sample's override
# (docker compose -f docker-compose.yml -f ../../samples/starter-mmo/docker-compose.override.yml up -d --build).
if [ "${1:-}" = "--starter-mmo" ]; then
    bot() { docker compose -f docker-compose.yml -f ../../samples/starter-mmo/docker-compose.override.yml run --rm -T bot --run-id "$RUN" --char-option class=warrior "$@"; }
    scenario "23 starter-mmo: Bat refused at 8 m, killed in one hit 2.5 m away, its corpse looted, back 30 s later" --scenario combat-bat
    scenario "24 starter-mmo: Guard: bad targets refused, cooldown, hits back, out of range refused" --scenario combat-rules
    summary
fi

# 0: kast_mapbake in the image (docs/services/world/maps.html#navmesh) bakes every tile of the mounted (read-only) hills into a temp dir;
# the server keeps the navtiles stored in the repo. Checksums are logged to compare with a Windows bake.
log="$LOGDIR/0.log"
echo "--- 0 kast_mapbake bakes maps/hills in the image"
docker compose run --rm -T --entrypoint sh bot -c \
    './kast_mapbake /app/maps/hills --out /tmp/nav && sha256sum /tmp/nav/* /app/maps/hills/.server/nav/*' 2>&1 |
    tee "$log" | grep -v -E '^ *(Container|Network) '
ok=${PIPESTATUS[0]}
grep -q '^4 navtile(s) -> /tmp/nav' "$log" || ok=1
record "0 kast_mapbake bakes maps/hills in the image" "$ok" "see $log"

scenario "1 register/verify/login + TLS + AuthOk" --scenario auth
scenario "2 character create/list/delete, limits, names" --scenario characters
scenario "3 characters are owner-only" --scenario ownership
scenario "4 AOI spawn/move/despawn" --scenario aoi
scenario "5 say chat in range only" --scenario chat
scenario "6 speedhack corrected, not replicated" --scenario speedhack
scenario "7 malformed/oversized/unknown frames" --scenario malformed
# root: reads auth's private key from its volume to mint an expired token (test only)
echo "--- 8 handshake rejections"
docker compose run --rm -T --user root bot --run-id "$RUN" --scenario handshake --auth-key /auth-keys/jwt-private.pem \
    2>&1 | tee "$LOGDIR/8.log" | grep -v -E '^ *(Container|Network) '
record "8 handshake rejections" "${PIPESTATUS[0]}" "see $LOGDIR/8.log"
scenario "9 duplicate login" --scenario duplicate
scenario "10a position persists across logout/login" --scenario persist
graceful "10b position persists across graceful restart" s10r restart

# 11: MySQL stopped mid-session
name="11 MySQL down mid-session"
echo "--- $name"
log="$LOGDIR/11.log"
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
bot --scenario dbdown >"$log" 2>&1 &
botpid=$!
ok=0
if wait_line "$log" 'READY stop-db' 120; then
    docker compose stop mysql >/dev/null 2>&1 && echo "  mysql stopped"
    if wait_line "$log" 'READY start-db' 180; then
        sleep 5 # the logged-out bot's save is now failing and being retried
        docker compose start mysql >/dev/null 2>&1 && echo "  mysql started"
    else
        ok=1
        docker compose start mysql >/dev/null 2>&1
    fi
else
    ok=1
fi
wait $botpid || ok=1
grep -v -E '^ *(Container|Network) ' "$log"
docker logs --since "$since" "$WORLD" 2>&1 | grep -E '"save failed|"database (unavailable|reconnected)"' | sed 's/^/  log: /'
wait_healthy mysql
wait_healthy auth
record "$name" $ok "see $log"

graceful "12 SIGTERM: server_shutdown, saves flushed, exit 0 < 10 s" s12 stop

# 13: Lua components with the sample lua/ (docs/services/world/lua.html): replication modes, then the
# persisted values across a relog (inside the scenario) and across a world restart.
name="13 Lua components: aoi/owner replication, persisted across relog and restart"
echo "--- $name"
log="$LOGDIR/13.log"
ok=0
bot --scenario components --tag s13 >"$log" 2>&1 || ok=1
grep -v -E '^ *(Container|Network) ' "$log"
read -r _ gold health < <(grep '^COMPONENTS' "$log") || ok=1
if [ $ok -eq 0 ]; then
    docker compose restart -t 15 world >/dev/null 2>&1 && echo "  world restarted"
    wait_healthy world || ok=1
    bot --scenario components-verify --tag s13 --expect-components "$gold,$health" 2>&1 | tee -a "$log" |
        grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || ok=1
fi
record "$name" $ok "see $log"

# 14: server events with the sample lua/systems/events.lua (docs/services/world/events.html)
scenario "14 Lua server events: sendTo/sendNearby/broadcast audiences" --scenario events

# 15: state machines with the sample lua/fsms/locomotion.lua + lua/systems/locomotion.lua (docs/services/world/state-machines.html)
scenario "15 state machines: transitions observed, denied one silent" --scenario fsm

# 16: NPCs with the sample lua/systems/npcs.lua (docs/services/world/npcs.html): the patrolling Guard seen, moving, leaving and returning
scenario "16 NPCs: Spawn{npc, Guard}, Health from its definition, moves, Despawn out of range, Spawn on return" --scenario npcs

# 17: areas with the sample maps' areas.json (docs/services/world/areas.html): start's cave_mouth portal to map 2, kept across a relog
scenario "17 portal: MapChange{2} first, Bat seen, B lost, relog stays on map 2" --scenario portal

# 18: NPC spawns (docs/services/world/npcs.html#npc-spawns): the test hook destroys the Guard; it respawns after the fixture's respawnSec (5 s)
scenario "18 NPC respawn: Guard destroyed by lua/systems/test_hooks.lua, back 5 s later at its spawn" --scenario respawn

# 19: ground on the sample hills map (docs/services/world/maps.html#ground): slope walk accepted and seen on the ground, fly hack corrected
scenario "19 terrain: hills_gate lands on the ground, slope walk accepted, fly hack -> Correction" --scenario terrain

# 20: navmesh on the sample hills map (docs/services/world/maps.html#navmesh): walking into the mesa's wall is corrected; the Sentry walks around it
scenario "20 navmesh: mesa wall -> Correction, Sentry patrols around it on the ground" --scenario navmesh

# 21-22: client events (docs/services/world/protocol.html#client-events) with the sample lua/systems/test_hooks.lua
scenario "21 client events: client.<name> reaches Lua with the sender; bad name / > 1 KB / non-object -> malformed" --scenario client-events
name="22 client events: 10 a second per session, the rest dropped and counted"
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
scenario "$name" --scenario client-event-rate
sleep 1 # the logout's despawn logs the count
if ! docker logs --since "$since" "$WORLD" 2>&1 | grep '"client events dropped"' | grep -q '"count":20'; then
    RESULTS[${#RESULTS[@]}-1]="FAIL  $name  no 'client events dropped' log with count 20"
    FAILED=1
    echo "==> FAIL $name (dropped count not logged)"
fi

summary
