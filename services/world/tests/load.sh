#!/usr/bin/env bash
# Load test: COUNT bots (default 500) running BEHAVIOUR (default wander) in one zone for DURATION seconds
# (default 300) against the compose stack. MAPS=1,2,4,5 spreads the bots evenly over those maps instead (each walks
# there through the portals: kast_bot --maps; an instance map among them, e.g. MAPS=1,4,5,6, spreads its share over
# instance copies, INSTANCE_SIZE bots each, default the map's maxPlayers: the crypt's 2). MAP_THREADS (default 1) is
# the world's. Recreates the world first so
# its run totals cover this test only (and it runs with this MAP_THREADS), and stops it gracefully afterwards to get
# the "tick stats total" line. LAYER_CAP=500 gives START_MAP "layerCap": 500 for this run only (a copy of the maps:
# tests/stack_override.sh); the result shows the peak live layers per map. COUNT + 100 over 1000 raises the world's
# MAX_CONNECTIONS to that for the run. With either, the world is recreated with the defaults afterwards.
# Pass: bot summary PASS, every 10 s window's tick p99 < 25 ms, run p99 < 25 ms, zero failed save attempts, and
# enter p99 (each bot's connect -> EnterWorldOk, the login storm's lobby latency) < 500 ms.
set -u
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1

COUNT="${COUNT:-500}"
DURATION="${DURATION:-300}"
BEHAVIOUR="${BEHAVIOUR:-wander}"
THREADS="${THREADS:-8}"
MAPS="${MAPS:-}"
INSTANCE_SIZE="${INSTANCE_SIZE:-}"
LAYER_CAP="${LAYER_CAP:-}"
export MAP_THREADS="${MAP_THREADS:-1}" # read by docker-compose.yml
RUN="${RUN_ID:-load$(date +%s)}"
LOG="build/load-$RUN.log"
P99_LIMIT_MS=25
ENTER_P99_LIMIT_MS=500
mkdir -p build
. tests/stack_override.sh
stack_override "$LAYER_CAP" "$( [ $((COUNT + 100)) -gt 1000 ] && echo $((COUNT + 100)) )"

docker compose up -d --no-deps --force-recreate world >/dev/null 2>&1
for i in $(seq 120); do
    [ "$(docker inspect -f '{{.State.Health.Status}}' "$(docker compose ps -q world)")" = healthy ] && break
    sleep 1
done
WORLD=$(docker compose ps -q world)
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)

echo "load: $COUNT x $BEHAVIOUR for ${DURATION}s on maps ${MAPS:-START_MAP}, MAP_THREADS=$MAP_THREADS${LAYER_CAP:+, LAYER_CAP=$LAYER_CAP} (run $RUN)"
docker compose run --rm -T bot --run-id "$RUN" --mode load --count "$COUNT" --behaviour "$BEHAVIOUR" \
    --duration "$DURATION" --threads "$THREADS" ${MAPS:+--maps "$MAPS"} ${INSTANCE_SIZE:+--instance-size "$INSTANCE_SIZE"} 2>&1 | grep -v -E '^ *(Container|Network) ' | tee "$LOG"
botrc=${PIPESTATUS[0]}

docker compose stop -t 15 world >/dev/null 2>&1 # emits "tick stats total"
logs=$(docker logs --since "$since" "$WORLD" 2>&1)
if [ -n "${COMPOSE_FILE:-}" ]; then
    stack_default
    docker compose up -d --no-deps world >/dev/null 2>&1 # back to the default maps and MAX_CONNECTIONS
else
    docker compose start world >/dev/null 2>&1
fi

echo
echo "---- world tick stats (10 s windows with players, then run total) ----"
echo "$logs" | grep '"tick stats' | grep -v '"population":0,' | sed 's/^/  /'
echo "$logs" | grep '"tick stats total"' | sed 's/^/  /'

worst=$(echo "$logs" | grep '"msg":"tick stats"' | grep -v '"population":0,' | sed -E 's/.*"p99_ms":([0-9.]+).*/\1/' | sort -g | tail -1)
total=$(echo "$logs" | grep '"tick stats total"' | sed -E 's/.*"p99_ms":([0-9.]+).*/\1/')
failed=$(echo "$logs" | grep '"shutdown complete"' | sed -E 's/.*"saves_failed_attempts":([0-9]+).*/\1/')
lost=$(echo "$logs" | grep '"saves not flushed"' | sed -E 's/.*"pending":([0-9]+).*/\1/')
echo "$logs" | grep -q '"shutdown complete"' && lost=0
enter50=$(grep '"phase":"summary"' "$LOG" | sed -E 's/.*"enter_ms_p50":([0-9.]+).*/\1/')
enter99=$(grep '"phase":"summary"' "$LOG" | sed -E 's/.*"enter_ms_p99":([0-9.]+).*/\1/')
windows=$(echo "$logs" | grep '"msg":"tick stats"' | grep -c -v '"population":0,')
# Live layers per map over the run: the base plus layer created - layer destroyed, at its peak.
layers=$(echo "$logs" | grep -E '"msg":"layer (created|destroyed)"' | awk '{
    m = $0; sub(/.*"map":/, "", m); sub(/[,}].*/, "", m)
    cur[m] += /layer created/ ? 1 : -1; if (cur[m] > peak[m]) peak[m] = cur[m]
} END { for (m in peak) printf "%smap %s: %d", (n++ ? ", " : ""), m, peak[m] + 1 }')

ok=0
echo
echo "---- result ----"
echo "  bot exit code:               $botrc"
echo "  windows with players:        $windows"
echo "  worst window tick p99:       ${worst:-?} ms (limit $P99_LIMIT_MS)"
echo "  run total tick p99:          ${total:-?} ms (limit $P99_LIMIT_MS)"
echo "  failed save attempts:        ${failed:-?}"
echo "  lost saves on shutdown:      ${lost:-?}"
echo "  enter p50 / p99:             ${enter50:-?} / ${enter99:-?} ms (p99 limit $ENTER_P99_LIMIT_MS)"
[ -z "$layers" ] && layers=$( [ -n "$LAYER_CAP" ] && echo "1 (no layer created)" || echo "none (no layerCap)")
echo "  peak layers:                 $layers"
[ "$botrc" -eq 0 ] || ok=1
awk -v w="${worst:-999}" -v t="${total:-999}" -v l=$P99_LIMIT_MS 'BEGIN { exit !(w < l && t < l) }' || ok=1
[ "${failed:-1}" = 0 ] || ok=1
awk -v e="${enter99:-99999}" -v l=$ENTER_P99_LIMIT_MS 'BEGIN { exit !(e < l) }' || ok=1
echo "$( [ $ok -eq 0 ] && echo PASS || echo FAIL ) load test"
exit $ok
