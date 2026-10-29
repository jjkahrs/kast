# Sourced by load.sh and integration.sh (from services/world): one run's compose override, build/stack-override.yml,
# which every later `docker compose` call picks up through COMPOSE_FILE (the bot's `compose run` too, so it never
# recreates the world with other settings).
#   stack_override CAP MAXCONN: CAP (empty: none) mounts a copy of tests/project/maps with "layerCap": CAP on
#     START_MAP's map.json, for the world and the bot (docs/services/world/maps.html#layers); MAXCONN (empty: none)
#     sets the world's MAX_CONNECTIONS. Both empty: stack_default.
#   stack_default: docker-compose.yml alone again.
stack_override() {
    local cap=$1 maxconn=$2 out=build/stack-override.yml start
    [ -z "$cap$maxconn" ] && { stack_default; return; }
    mkdir -p build
    {
        echo "services:"
        echo "  world:"
        [ -n "$maxconn" ] && printf '    environment:\n      MAX_CONNECTIONS: "%s"\n' "$maxconn"
        [ -n "$cap" ] && printf '    volumes:\n      - ./build/layer-maps:/app/maps:ro\n  bot:\n    volumes:\n      - ./build/layer-maps:/app/maps:ro\n'
    } > "$out"
    if [ -n "$cap" ]; then
        rm -rf build/layer-maps && cp -r ../../tests/project/maps build/layer-maps
        start=$(grep -l "\"id\": *${START_MAP:-1}," build/layer-maps/*/map.json | head -1)
        sed -i "0,/{/s//{\"layerCap\": $cap,/" "$start"
        echo "  layers: $start has \"layerCap\": $cap for this run"
    fi
    export COMPOSE_PATH_SEPARATOR=: COMPOSE_FILE="docker-compose.yml:$out"
}

stack_default() { unset COMPOSE_FILE COMPOSE_PATH_SEPARATOR; }
