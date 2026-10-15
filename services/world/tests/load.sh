#!/usr/bin/env bash
# Load test: COUNT bots (default 500) running BEHAVIOUR (default wander) in one zone for DURATION seconds
# (default 300) against the compose stack. Restarts the world first so its run totals cover this test only, and
# stops it gracefully afterwards to get the "tick stats total" line.
# Pass: bot summary PASS, every 10 s window's tick p99 < 25 ms, run p99 < 25 ms, zero failed save attempts.
set -u
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1

COUNT="${COUNT:-500}"
DURATION="${DURATION:-300}"
BEHAVIOUR="${BEHAVIOUR:-wander}"
THREADS="${THREADS:-8}"
RUN="${RUN_ID:-load$(date +%s)}"
LOG="build/load-$RUN.log"
P99_LIMIT_MS=25
mkdir -p build

docker compose restart world >/dev/null 2>&1
for i in $(seq 120); do
    [ "$(docker inspect -f '{{.State.Health.Status}}' "$(docker compose ps -q world)")" = healthy ] && break
    sleep 1
done
WORLD=$(docker compose ps -q world)
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)

echo "load: $COUNT x $BEHAVIOUR for ${DURATION}s (run $RUN)"
docker compose run --rm -T bot --run-id "$RUN" --mode load --count "$COUNT" --behaviour "$BEHAVIOUR" \
    --duration "$DURATION" --threads "$THREADS" 2>&1 | grep -v -E '^ *(Container|Network) ' | tee "$LOG"
botrc=${PIPESTATUS[0]}

docker compose stop -t 15 world >/dev/null 2>&1 # emits "tick stats total"
logs=$(docker logs --since "$since" "$WORLD" 2>&1)
docker compose start world >/dev/null 2>&1

echo
echo "---- world tick stats (10 s windows with players, then run total) ----"
echo "$logs" | grep '"tick stats' | grep -v '"population":0,' | sed 's/^/  /'
echo "$logs" | grep '"tick stats total"' | sed 's/^/  /'

worst=$(echo "$logs" | grep '"msg":"tick stats"' | grep -v '"population":0,' | sed -E 's/.*"p99_ms":([0-9.]+).*/\1/' | sort -g | tail -1)
total=$(echo "$logs" | grep '"tick stats total"' | sed -E 's/.*"p99_ms":([0-9.]+).*/\1/')
failed=$(echo "$logs" | grep '"shutdown complete"' | sed -E 's/.*"saves_failed_attempts":([0-9]+).*/\1/')
windows=$(echo "$logs" | grep '"msg":"tick stats"' | grep -c -v '"population":0,')

ok=0
echo
echo "---- result ----"
echo "  bot exit code:               $botrc"
echo "  windows with players:        $windows"
echo "  worst window tick p99:       ${worst:-?} ms (limit $P99_LIMIT_MS)"
echo "  run total tick p99:          ${total:-?} ms (limit $P99_LIMIT_MS)"
echo "  failed save attempts:        ${failed:-?}"
[ "$botrc" -eq 0 ] || ok=1
awk -v w="${worst:-999}" -v t="${total:-999}" -v l=$P99_LIMIT_MS 'BEGIN { exit !(w < l && t < l) }' || ok=1
[ "${failed:-1}" = 0 ] || ok=1
echo "$( [ $ok -eq 0 ] && echo PASS || echo FAIL ) load test"
exit $ok
