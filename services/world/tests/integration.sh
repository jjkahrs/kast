#!/usr/bin/env bash
# Runs every integration scenario against the compose stack (docker compose up -d --build first).
# Prints PASS/FAIL per scenario and exits non-zero if any failed.
# Scenarios 10-12 stop/restart containers, so run this against a test stack only.
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

echo
echo "================ integration summary ($RUN) ================"
printf '%s\n' "${RESULTS[@]}"
exit $FAILED
