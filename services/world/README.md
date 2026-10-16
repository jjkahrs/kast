# Kast World Server

MMORPG world server for Kast, written in C++20. It covers the following:

- **Connections:** clients connect over TLS-over-TCP and log in with an access token from the [auth service](../auth/README.md).
- **Characters:** clients create, list and delete characters, then enter the map they were saved on (see "Maps").
- **Movement:** the server validates every move (map bounds and max speed) and replicates it to nearby players through grid-based interest management.
- **Chat:** "say" chat reaches everyone in range.
- **Lua:** game-defined components, state machines and server-authoritative systems on the tick, persisted per character and replicated to clients (see "Lua components and systems").
- **NPCs:** server-authoritative NPCs come from per-tile spawn files (with respawn timers and patrol paths) or from Lua systems, which move and destroy them; they reach players through the same interest management, with their components and state machines (see "NPCs" and "NPC spawns").
- **Architecture:** an EnTT ECS runs on a fixed 20 Hz tick, and characters persist to MySQL on a worker thread, so the tick never waits on the database.

The wire format is in [`PROTOCOL.md`](PROTOCOL.md).

| Dir | What |
|---|---|
| `protocol/` | Static lib: frame codec, opcodes and message structs. No Asio, DB or ECS, so a game client can reuse it |
| `map/` | Static lib `kast_map`: `map.json` parsing and tile math. Standard library + nlohmann_json only, no file IO, so the engine and tools can reuse it |
| `server/` | `kast_world`: `core/` (config, log, clock, limits), `net/` (TLS, sessions), `auth/` (JWKS, JWT), `db/` (MySQL, migrations, worker), `world/` (ECS components and systems), `lua/` (Lua sandbox, component and state machine definitions, systems runtime, event bus, state machines), `app/` (glue) |
| `bot/` | `kast_bot`: headless test client covering auth HTTP, Mailpit verification, TLS, scripted scenarios and load mode |
| `migrations/` | `NNN_name.sql`, applied in order at startup |
| `lua/` | Sample Lua components, state machines and systems (`LUA_ROOT` in the image) |
| `tests/` | `self_check.cpp` + `lua_checks.cpp` (ctest), `integration.sh`, `load.sh`, `docs_check.sh` |

## Setup

```sh
cd services/world
cp .env.example .env              # dev defaults; change the passwords
docker compose up -d --build      # world + auth (built from ../auth) + MySQL 8.4 + Mailpit
docker compose ps                 # all healthy
docker compose logs world         # "migration applied", "listening"
```

| Service | Host port | Notes |
|---|---|---|
| `world` | 7777 | Applies migrations, then listens. The dev TLS cert is in volume `world-certs` |
| `auth` | 3100 | `services/auth`, unmodified. **`TRUST_PROXY=true`, for testing only** (see Security) |
| `mysql` | 3307 | One MySQL 8.4 for both services (volume `mysql-data`) |
| `mailpit` | 8026 | Verification emails. The bots read them through the API |

- **Side by side with auth:** the host ports differ from `services/auth`'s own stack, so both stacks can run at once.
- **Database setup:** the mysql image creates auth's database and user from `MYSQL_DATABASE`/`MYSQL_USER`. On a fresh volume, `docker/mysql-init.sh` also creates `kast_world`, `kast_world_test` and the world user.
  - `kast_world_test` is reserved for database tests. The self-check needs no database.
- **Stopping:** `docker compose down` keeps the data. `down -v` also deletes the databases, auth's signing key and the dev certificate.

### Building on Windows (MSVC + vcpkg)

From a VS 2022 x64 developer prompt, with `VCPKG_ROOT` set:

```bat
cmake --preset debug          :: or release; static triplet x64-windows-static
cmake --build --preset debug
ctest --preset debug          :: world_self_check, mapbake_cli
```

The Linux image builds with the `linux-release` preset (`x64-linux-release` triplet). Its `Dockerfile` stages are:
- `deps`: vcpkg at the pinned baseline. This stage is cached until `vcpkg.json` changes. The first build compiles the MySQL client from source, which is slow but happens only once.
- `build`: compiles with warnings as errors, then runs ctest.
- runtime: a slim image running as the non-root `world` user, with `kast_world`, `kast_bot` and `kast_mapbake`.

To run the server on the host against the compose MySQL and auth, point `.env` at `localhost:3307` and `localhost:3100` (the `.env.example` defaults), then run `build/debug/kast_world.exe` from `services/world`.

## Commands

| Command | What |
|---|---|
| `ctest --preset debug` | Self-check: codec round trip for every message, frame rejection, names, movement, AOI, chat, rate limits, config, tick stats, JWT and JWKS with local RSA keys. Lua (`tests/lua_checks.cpp`): definition parsing (every type, option, new key and error case), reserved and duplicate names, sandbox, `kast.*` semantics and every `set` error, hook order, an error disabling one system, startup errors listed in full, persistence round trip + drift + verbatim preservation, and replication routing (owner / aoi / none, AOI-enter full set, unchanged not resent, nothing after `Despawn`, oversize). NPCs: `kast.create` / `kast.destroy` and every bad argument, `MAX_NPCS` (and `0`), the `Npc` / `Transform` built-in rules and clamping, `query` by `Character` / `Npc`, `Spawn{kind, template}` / `EntityMove` / `Despawn` routing as an NPC moves across the grid, aoi / owner components and `sendTo` / `sendNearby` on NPCs, never saved or dirty, Lua components and `kast.fsm` on NPCs, no `onEnter` / `onLeave`, a destroy in the tick it came into view, silent removal at shutdown, and the sample `lua/` (the Guard from its spawn file patrolling at 3 m/s). NPC spawns: `parse_spawns` and every spawn file error, respawn timing on the injected clock, `MAX_NPCS` deferral, the `Spawn` built-in. Protocol v2: the `Spawn` layout for both kinds, a bad kind and a v1 `Spawn` rejected as malformed State machines: definition parsing and every rejection, duplicate names, `GAME_STATE_MACHINES` startup errors, `allowedFrom` / omitted / `"*"` / self-transition rules, callback and event order, queued changes, callback errors and timeouts, game machine startup, the `StateMachine` built-in lifecycle (not persisted or replicated), every `kast.fsm` misuse, read-only `kast.fsm`, and C++ / Lua parity |
| `docker compose build` | Linux build; the self-check runs inside it |
| `bash tests/integration.sh` | All 18 integration scenarios (1–18) against the running stack. Prints PASS/FAIL each and exits non-zero on any failure |
| `bash tests/load.sh` | Load test: 500 `wander` bots for 300 s (`COUNT`, `DURATION`, `BEHAVIOUR`, `THREADS` override) |
| `bash tests/docs_check.sh` | Every opcode, code and env var in the source appears in PROTOCOL.md / README.md |
| `docker compose run --rm bot --scenario aoi` | One scenario |
| `docker compose run --rm bot --mode load --count 50 --behaviour chat --duration 60` | Ad-hoc load |

**Scenario names:** `auth`, `characters`, `ownership`, `aoi`, `chat`, `speedhack`, `malformed`, `handshake`, `duplicate`, `persist`, `shutdown`, `restart-verify`, `dbdown`, `components`, `components-verify` (`--expect-components gold,health`, after a restart), `events`, `fsm`, `npcs`.

**Scenario 13 (`components`):** needs the sample `lua/`. A and B are in range. A gets its own `Health` and `Wallet`; B gets A's `Health` but never A's `Wallet`; `Spawn` comes before data; unchanged values are not resent; nothing follows `Despawn`. A walks and its `Health` drains. After a relog, A's `Health` and `Wallet` were persisted. A client `ComponentUpdate` gets `unexpected_message`. Then `world` restarts and `components-verify` checks both values again.

**Load behaviours:** `idle`, `wander` (random valid moves at 10 Hz, 9 m/s), `chat`, `speedhack` (50 m/s, expects `Correction`), `malformed` (expects `unknown_opcode`).

**Running the scripts:** they need bash, which is Git Bash on Windows. `integration.sh` stops and restarts `world` and `mysql` for scenarios 10–13, so only point it at a test stack.

### Migrations

- **Order:** `migrations/*.sql` run in filename order at every start, each once, and are tracked in `schema_migrations`.
- **Locking:** a MySQL named lock (`GET_LOCK('kast_world_migrate')`) stops two instances from migrating at once.
- **Statements:** a statement ends with `;` at the end of a line, and every statement runs as a prepared statement.
- **Failures:** startup fails, with a non-zero exit, if the database is unreachable or a migration fails. MySQL DDL auto-commits, so a file that fails halfway must be fixed by hand.

## Configuration

- **Source:** every setting comes from an environment variable, validated at startup.
- **Invalid values:** the server lists every bad or missing variable and exits with status 1.
- **Env files:** `.env.example` is committed; `.env` is gitignored.
- **Compose overrides:** in compose, `DB_HOST`, `DB_PORT`, `AUTH_JWKS_URL`, `WORLD_PORT`, `TLS_DEV_CERT_DIR`, `MIGRATIONS_DIR`, `LUA_ROOT` and `MAP_ROOT` are overridden with in-network values. `GAME_STATE_MACHINES` defaults to the sample's `zone_cycle` unless `.env` sets it.

| Variable | Default | Notes |
|---|---|---|
| `REALM_ID` | **required** | Should match `realms.id` in auth (not checked). Every character row is keyed by it |
| `WORLD_HOST` | `0.0.0.0` | Bind address |
| `WORLD_PORT` | `7777` | TLS game port |
| `MAX_CONNECTIONS` | `1000` | Beyond this: `Disconnect{server_full}` |
| `MAX_FRAME_BYTES` | `4096` | Max frame payload |
| `HANDSHAKE_TIMEOUT_MS` | `10000` | TLS + Hello + Auth must finish within this |
| `IDLE_TIMEOUT_MS` | `60000` | Nothing received for this long: `idle_timeout` |
| `PING_INTERVAL_MS` | `20000` | Server heartbeat `Ping` |
| `MSG_RATE_PER_SEC` | `60` | Per-connection message rate |
| `MSG_BURST` | `120` | Token-bucket burst |
| `MSG_RATE_GRACE_MS` | `2000` | Continuous excess for this long: `rate_limited` |
| `MAX_SEND_BUFFER_BYTES` | `1048576` | A client that stops reading is dropped past this |
| `TLS_CERT_FILE` / `TLS_KEY_FILE` | *(empty)* | PEM certificate chain and key. Set both, or neither for the dev cert |
| `TLS_DEV_CERT_DIR` | `./certs` | Dev cert (`server.crt`/`server.key`, EC P-256, SANs localhost/world/127.0.0.1), generated on first start and reused after. Gitignored |
| `AUTH_JWKS_URL` | **required** | e.g. `http://auth:3000/.well-known/jwks.json` |
| `JWT_ISSUER` | `kast-auth` | Must equal auth's `JWT_ISSUER` |
| `JWT_AUDIENCE` | `kast` | Must equal auth's `JWT_AUDIENCE` |
| `JWT_CLOCK_SKEW_SECONDS` | `5` | Leeway on `exp` |
| `JWKS_REFRESH_SECONDS` | `300` | Matches auth's `max-age=300` |
| `JWKS_MIN_REFETCH_SECONDS` | `10` | An unknown `kid` refetches the JWKS at most this often |
| `JWKS_TIMEOUT_MS` | `3000` | JWKS HTTP connect/read timeout |
| `DB_HOST` | **required** | |
| `DB_PORT` | `3306` | |
| `DB_USER` / `DB_PASSWORD` / `DB_NAME` | **required** | World database (`kast_world`), never auth's |
| `DB_CONNECT_TIMEOUT_SECONDS` | `3` | Reads and writes time out after 5 s |
| `MIGRATIONS_DIR` | `./migrations` | |
| `TICK_HZ` | `20` | Fixed simulation rate |
| `MAP_ROOT` | `/app/maps` | Folder with one `<folder>/map.json` per map (see "Maps"). A bad manifest, a duplicate id or no maps refuses the start. `.env.example` sets the test project's `maps/`; compose mounts it at `/app/maps` |
| `START_MAP` | `1` | Map id new characters start on, at its `spawn`; also where a character whose saved map is gone goes. Must be loaded |
| `MIN_Y` / `MAX_Y` | `-100` / `500` | y is checked against the ground (see "Ground"), then clamped to this range |
| `MAX_SPEED` | `10` | m/s on x/z |
| `SPEED_TOLERANCE` | `1` | m/s on top of `MAX_SPEED` |
| `MOVE_BURST_SECONDS` | `1` | Movement budget cap, in seconds of travel. Absorbs jitter |
| `MOVE_GROUND_TOLERANCE` | `0.5` | Metres a move may end below the ground as-is; deeper is lifted onto it (see "Ground") |
| `MOVE_MAX_AIR` | `6` | Metres above the ground a move may not rise past or hover at; falling is always allowed (see "Ground") |
| `MOVE_NAV_CHECK` | `1` | `1`: a move must end on the map's navmesh, if it has navtiles (see "Navmesh"); `0`: no navmesh check |
| `AOI_CELL_SIZE` | `64` | Interest grid cell, in metres. Must divide the 256 m tile size |
| `AOI_VIEW_RADIUS` | `128` | Players within this distance see each other and the NPCs |
| `MAX_CHARS_PER_ACCOUNT` | `10` | Live characters per account per realm |
| `MAX_NPCS` | `10000` | NPCs in the world (all maps) at once, 0–100000. `kast.create` past it raises an error |
| `CHAT_MAX_BYTES` | `256` | UTF-8 bytes per `Say` |
| `CHAT_RATE_COUNT` / `CHAT_RATE_WINDOW_MS` | `5` / `10000` | Say rate limit |
| `SAVE_INTERVAL_MS` | `60000` | Periodic save of characters whose position changed |
| `SAVE_RETRY_MIN_MS` / `SAVE_RETRY_MAX_MS` | `500` / `30000` | Exponential backoff for failed saves |
| `SHUTDOWN_TIMEOUT_MS` | `10000` | Graceful-shutdown budget, from the signal to exit |
| `TICK_STATS_INTERVAL_SECONDS` | `10` | How often tick p50/p99/max is logged |
| `LUA_ROOT` | `/app/lua` | Folder with `components/`, `fsms/` and `systems/` (see "Lua components and systems"). Missing or empty: no Lua. `.env.example` sets `./lua` for running on the host |
| `GAME_STATE_MACHINES` | *(empty)* | Comma-separated names of `fsms/` definitions to run as game state machines (see "State machines"). Empty or unset: none. A name with no valid definition refuses the start. `.env.example` and compose set `zone_cycle` |
| `LOG_LEVEL` | `info` | `debug`, `info`, `warn`, `error` |

**Compose only** (`docker-compose.yml`, not read by the server):

| Variable | Default | Notes |
|---|---|---|
| `MYSQL_ROOT_PASSWORD` | **required** | |
| `AUTH_DB_PASSWORD` | **required** | |
| `AUTH_DB_USER` / `AUTH_DB_NAME` | `kast_auth` | |
| `TEST_DB_NAME` | `kast_world_test` | |
| `WORLD_HOST_PORT` / `AUTH_HOST_PORT` / `MYSQL_HOST_PORT` / `MAILPIT_HOST_PORT` | `7777` / `3100` / `3307` / `8026` | |

## Security behaviour

### TLS

- **Protocol:** TLS 1.2 or later only.
- **Production:** set `TLS_CERT_FILE` and `TLS_KEY_FILE`.
- **Dev certificate:** without them, the server generates a self-signed dev cert once and logs a warning (the same idea as auth's dev signing key). `TLS_DEV_CERT_DIR` is gitignored.
- **Bot verification:** the bot checks the server certificate and host name against the file passed with `--ca` (`/certs/server.crt` in compose).
- **No downgrade:** a client that never completes TLS is closed when the handshake deadline expires. Plain TCP never gets a game message.

### Token verification

The server follows the "Verifying access tokens from the World Server" section of [`../auth/README.md`](../auth/README.md):

- **Algorithm:** `alg` must be `RS256`, so `none` and HS* are rejected. HS256 signed with the public key (key confusion) is rejected too.
- **Key selection:**
  - The key is chosen by the `kid` header from the cached JWKS.
  - The JWKS is refreshed after 300 s.
  - An unknown `kid` refetches the JWKS once, at most every `JWKS_MIN_REFETCH_SECONDS`, so random `kid`s can't hammer auth.
- **Claims:**
  - `iss` and `aud` must match the config.
  - `exp` is required, with `JWT_CLOCK_SKEW_SECONDS` of leeway.
  - `sub` must be a decimal account id.
  - `role` must be `player`, `gm` or `admin`.
- **Auth unreachable:** if the JWKS can't be fetched, the client gets `service_unavailable`.
- **Off the game thread:** verification and the JWKS fetch run on their own thread, never on the game thread.
- **Logging:** tokens are never logged, and neither are passwords. Chat text is never logged at info or above; at debug level only its byte length is.

### Ban gap

- **When the token is checked:** only at the handshake. A session then lasts until disconnect.
- **Effect:** a player banned in auth keeps playing until they disconnect. They can't log in again once their access token expires, at most 15 minutes after it was issued, because auth revokes the refresh tokens immediately.
- **Closing the gap:** this needs a ban/revocation endpoint in auth that the world polls or subscribes to. Neither exists yet.
- **Role changes:** a role change also only applies at the next login.

### Abuse limits

All of these are configurable; see PROTOCOL.md for the exact rules.
- **Frames:** maximum frame size, handshake deadline and idle timeout.
- **Messages:** the per-connection message rate, with disconnect on sustained excess, and one character request in flight at a time.
- **Movement:** bounds, speed and the ground (no flying), with `Correction`. Rejected moves are never replicated.
- **Chat:** size limit, UTF-8 validation, control-character stripping and the 5 per 10 s rate limit.
- **Connections:** a connection limit and a send-buffer cap for clients that stop reading.
- **Malformed input:** an unknown opcode, a bad length or a decode failure disconnects the client with a reason code.

### TRUST_PROXY (test only)

- **What it does:** the test compose sets `TRUST_PROXY=true` on auth. Every bot sends its own `X-Forwarded-For` address, so auth's per-IP limits (register 5/h, login 20/min) apply per bot. That lets one harness register, verify and log in 500+ accounts.
- **Never in production:** don't set it on an auth service that clients can reach without a trusted proxy in front of it. Anyone could then forge their IP and bypass the limits.
- **Auth's key in scenario 8:** the test bot also mounts auth's key volume read-only and runs as root for that one scenario. It uses the key to mint an expired but correctly signed token. This is test-only too.

## Maps

The world is a set of maps (continents, dungeons), all run by this one process. A map is seamless inside; maps are
joined only by transfers. Players and NPCs on different maps never see each other, even at the same x/z.

```
<MAP_ROOT>/<folder>/map.json        one folder per map; dot-folders (.server/) are skipped
<MAP_ROOT>/<folder>/areas.json      optional: the map's named areas (see Areas)
<MAP_ROOT>/<folder>/heights/<tx>_<tz>.r16|.png   optional: tile heightmaps (engine README "Terrain"; see Ground)
<MAP_ROOT>/<folder>/.server/spawns/<tx>_<tz>.json   optional, server only: the NPCs a tile places (see NPC spawns)
<MAP_ROOT>/<folder>/.server/nav/<tx>_<tz>.navtile   optional, server only: the tile's baked navmesh (see Navmesh)
```

```json
{ "version": 1, "id": 1, "name": "Eastlands",
  "tiles": [[0, 0, 7, 7]],
  "heightRange": [-100, 500],
  "spawn": {"pos": [1024, 0, 1024], "heading": 0},
  "material": "materials/terrain_grass.mat",
  "nav": {"agentRadius": 0.4, "agentHeight": 1.8, "maxClimb": 0.5, "maxSlopeDeg": 45, "cellSize": 0.5} }
```

- **Tiles:** a map is made of 256 m tiles; tile `(tx, tz)` = `(floor(x / 256), floor(z / 256))`, so x = 256 is tile 1.
  The map's origin is the corner of tile (0, 0). `tiles` lists inclusive rectangles `[x0, z0, x1, z1]`; the map is
  their union, and negative tiles are fine.
- **Identity:** `id` (0-65535) is what the database, the wire and Lua use. It must be unique across maps; the folder
  name only locates the files. `version` must be 1.
- **Required:** `version`, `id`, `tiles` and `spawn.pos` (on a declared tile). `name`, `spawn.heading`, `heightRange`,
  `material` and `nav` are optional. The game client's terrain uses `heightRange`, `material` and `nav.maxSlopeDeg`
  (engine README "Terrain"); the server uses `heightRange` and the heightmaps for the [ground](#ground). `map/heights.h` (heightmap decoding and the shared
  ground triangles) is part of `kast_map`, so the server build needs vcpkg's `stb`.
- **Startup:** every map is loaded and logged (`maps loaded`). Any bad manifest (with the file and the problem, such as
  `maps/cave/map.json: tiles[0]: x0 <= x1 and z0 <= z1 required`), a duplicate id, no maps, a missing `START_MAP`, or a
  spawn y outside `MIN_Y`/`MAX_Y` refuses the start.
- **Bounds:** a move onto a tile the map does not declare is rejected with `Correction`.
- **Characters:** `characters.map_id` (`migrations/003_character_map.sql`) is saved with the position. On enter, a map
  that is no longer loaded, or a position no longer on its map, puts the character at `START_MAP`'s spawn (logged).
  Rows from before the migration have `map_id` 0, read as `START_MAP` with their position kept.
- **Transfers:** `World::transfer(e, {map, pos, heading})` (and `kast.transfer` from Lua) is the only way to change map,
  for players and NPCs alike, and also teleports within a map. It moves the entity at once, resets a player's move
  budget and sends it `MapChange`, then `Despawn` for its old neighbours, who get its `Despawn`. The next interest pass
  sends the `Spawn`s at the destination: in the same tick for a [portal](#areas), the next tick for `kast.transfer`. So
  `MapChange` always reaches the client first ([`PROTOCOL.md`](PROTOCOL.md#maps)).
- **Ownership:** a transfer to a tile this process does not own is refused and logged. Every loaded tile is owned for
  now; the check is the seam for running maps in several processes later.

### Ground

The server has no physics engine; it knows the ground from the same heightmaps the client stands on.

- **Loading:** at startup, each owned tile's `heights/<tx>_<tz>.r16` (tried first) or `.png` is decoded with
  `kast_map`'s `decode_heights`. A tile without a file is flat at the map's `heightMin` (0 without `heightRange`). A
  file that fails to decode refuses the start (`maps/hills/heights/0_0.png: png: 8-bit with 1 channel(s), need 16-bit
  grayscale`).
- **Height:** `Maps::ground(map, x, z)` is `kast_map`'s `ground_at`: the triangle under the point, the same triangles as
  the client's Jolt heightfield, so client and server agree to within float rounding. Off the map's tiles it is empty.
- **Player moves:** after the speed check, against the ground under the new position:
  - more than `MOVE_GROUND_TOLERANCE` (0.5 m) below it: accepted and lifted onto it;
  - more than `MOVE_MAX_AIR` (6 m) above it and not lower than the last position: rejected (`Correction`), so no
    flying or hovering;
  - lower than the last position: accepted, however high (falling).
- **Placement:** NPCs always stand on the ground (`kast.create`, spawn files and `kast.set` of `Transform` ignore y).
  Transfers and portals put players and NPCs on the ground at the destination. A character saved under the ground
  (the heights changed since) enters on it.
- **Lua:** `kast.map.ground(map, x, z)` is the height there, or `nil` off the map.

### Navmesh

The ground says how high a player is, not whether they could have walked there. A map can carry a baked navmesh
(Recast/Detour) of where its agents can walk; the server checks player moves against it and NPCs path on it.

- **Baking:** `kast_mapbake <mapDir> [--out <dir>]` (built with the server, on Windows and in the image) bakes one
  Recast tile per declared tile into `<mapDir>/.server/nav/<tx>_<tz>.navtile` (or `--out`). Run it after editing a
  map's heights, then commit the navtiles; the editor's Play and the server need no tool.
  ```sh
  build/debug/kast_mapbake.exe ../../tests/project/maps/hills
  # 0_0.navtile: 40 polys, 27452 bytes, 0.97 s   (debug build; one line per tile)
  # 4 navtile(s) -> ../../tests/project/maps/hills/.server/nav
  docker compose run --rm -T --entrypoint ./kast_mapbake bot /app/maps/hills --out /tmp/nav   # in the image
  ```
  - **Input:** each tile's ground triangles (`map::for_each_triangle`, the triangles the client renders and Jolt
    collides with) plus a border from its neighbours on the map, so paths cross tile edges. A tile without heights is
    flat.
  - **Params:** the manifest's `nav` (defaults `agentRadius` 0.4, `agentHeight` 1.8, `maxClimb` 0.5, `maxSlopeDeg` 45,
    `cellSize` 0.5; cell height is `cellSize / 2`). `maxSlopeDeg` is also the client's character max slope on terrain,
    so the bake and Jolt agree on what is walkable. `cellSize` must divide 256, and `tan(maxSlopeDeg) * cellSize`
    should not exceed `maxClimb`, or walkable slopes come apart.
  - **Lint first:** height seams (a tile's edge samples must equal its neighbour's) and every spawn file
    (`map::parse_spawns`, tile on the map, ids unique). Any error is printed (`seam 0_0 | 1_0: 257 samples differ,
    worst 1.53 m at z 0`) and the tool exits 1 having written nothing. Usage errors exit 2.
  - **Cost:** with `cellSize` 0.5, about 1 s and 30-45 KB per hills tile (40-90 polygons).
- **Navtile format:** a 12-byte little-endian header, `{'KNAV', u32 version (1), u32 Detour DT_NAVMESH_VERSION (7)}`,
  then the Detour tile from `dtCreateNavMeshData` for tile (tx, tz) (`map/navtile.h`).
- **Loading:** at startup, each owned tile's navtile goes into one `dtNavMesh` per map (`server/world/nav.cpp`, the only
  file that includes Detour), sized for the map's declared tiles. A bad magic, an unknown navtile or Detour version, a
  tile baked for another tile, or too many polygons refuses the start with the file and the reason
  (`maps/hills/.server/nav/0_0.navtile: navtile version 99, this server reads 1 (rebake with kast_mapbake)`). A map
  without navtiles has no navmesh and keeps the [Ground](#ground) rules only.
- **Player moves:** with `MOVE_NAV_CHECK=1` (default), on a map with a navmesh, a move must end within 0.5 m across
  (the bake's `agentRadius` shrink at walkable edges) and `MOVE_MAX_AIR` up or down of a navmesh polygon. Otherwise
  the verdict is `off_mesh` and the client gets `Correction` at once, spending no move budget: walking up a cliff face
  or onto a slope steeper than `maxSlopeDeg` is snapped back. No grace period, so a player cannot drop off a cliff
  face either.
- **NPC paths:** `kast.map.path(map, from, to)` (`{x, y, z}` tables) is the straight path along the navmesh: a list of
  `{x, y, z}` points from `from` snapped onto the mesh to `to` snapped onto it, with the corners between. It is `nil`
  when the map has no navmesh, either end is more than 2 m across (or 4 m up or down) from it, or `to` cannot be
  reached. The sample `systems/npcs.lua` walks these corners between a patrol's points, so the hills' Sentry goes
  round the mesa its route crosses.
- **Limits:** Detour's poly refs are 32-bit here: tile bits + polygon bits <= 22, so a map of 1024 tiles allows 4096
  polygons per tile (more tiles, fewer polygons). A navtile over the limit refuses the start; `DT_POLYREF64` (Detour
  rebuilt) lifts it. No dynamic obstacles, off-mesh links (jumps, ladders) or static props in the bake, and the client
  does no pathing.
- **Sample:** `tests/project/maps/hills` has a mesa (x 170-200, z 316-346, 25 m flat top, 81 degree walls) and the
  Sentry (`.server/spawns/0_1.json`) patrolling from (150, 331) to (215, 331), straight through it. Its baked navtiles
  are in `.server/nav/`; ctest `mapbake_cli` fails if they no longer match a fresh bake.

## Areas

A map can name its regions (zones, subzones, towns) in an optional `areas.json` next to its `map.json`. The server
always knows which area every player is in. It publishes enter/leave events on the [event bus](#events), answers
area lookups from Lua, and treats an area with a `portal` as a zone line to another map.

```json
{ "version": 1, "areas": [
  {"id": "eastvale", "name": "Eastvale", "priority": 0, "rect": [0, 0, 2048, 2048],
   "flags": {"pvp": false, "noMount": false}, "music": "sounds/eastvale.ogg"},
  {"id": "goldshire", "name": "Goldshire", "parent": "eastvale", "priority": 10,
   "polygon": [[1000, 980], [1035, 980], [1035, 1030], [1000, 1030]], "y": [-50, 200], "flags": {"pvp": true}},
  {"id": "cave_mouth", "name": "Cave Entrance", "parent": "eastvale", "priority": 20, "rect": [1040, 990, 1050, 1000],
   "portal": {"map": 2, "pos": [256, 0, 240], "heading": 3.14}}
]}
```

- **Shape:** exactly one of `rect` `[x0, z0, x1, z1]` (x0 < x1, z0 < z1) or `polygon` (at least 3 `[x, z]` points, any
  simple polygon, convex or not), on the x/z plane. An optional `y` `[min, max]` limits it in height (bounds
  included). A point exactly on an edge may count as either side.
- **Which area:** a point is in the highest-`priority` area that contains it (default priority 0), or in none.
  `kast_map`'s `area_at` does the lookup with a per-tile index built at load. It is pure, so the client can reuse it.
- **Fields:** `id` (unique, required). `name` defaults to `id`. `music` is a project path (not inherited). `flags` is
  any JSON object: an area's flags are its `parent`'s (resolved the same way), with its own on top. The server only
  passes flags on. Game Lua decides what `pvp`, `noMount` and the rest mean.
- **Load errors** refuse the start, naming the file and area: a bad shape or field, a duplicate id, an unknown parent
  or a parent cycle, two areas with equal priority that overlap (sharing an edge is fine, and so are disjoint `y`
  ranges), a portal to an unknown map, a portal destination that is not on that map's tiles, or one inside a portal area
  there (so a portal never sends a player straight into another).
- **Events:** after movement each tick, each player that moved is looked up again. On a change the bus gets
  `area.leave` for the old area, then `area.enter` for the new one. The payload is `{entity, map, area, name, flags}`,
  where `area` is the area id and `flags` are the resolved flags. Leaving into no area publishes only `area.leave`. On
  login (and after any transfer) the player's area is set the same way, so login publishes `area.enter` alone; logout
  publishes nothing (`onLeave` covers it). NPCs get no area events.
- **Portals:** a player who moves into a portal area is sent to `portal.map` at `portal.pos` / `portal.heading` through
  `World::transfer`, before the tick's interest pass. The client gets `MapChange`, then `Despawn` for its old
  neighbours, then `Spawn` for its new ones, all in that tick. The bus gets `area.enter` for the portal area, then
  `area.leave` for it and `area.enter` for the destination's area. The player is not in a portal on arrival, so it
  must walk out and back in to return.

| Lua | |
|---|---|
| `kast.map.area(e)` | `{id, name, flags, music}` of the area player or NPC `e` is in, or `nil` |
| `kast.map.areaAt(map, x, y, z)` | The same for a point on loaded map `map`, or `nil`. Raises on an unloaded map |

## Persistence

- **Schema:** characters live in `kast_world.characters` with these fields:
  - `id`, `realm_id`, `account_id` (the JWT `sub`) and `name`
  - `pos_x/y/z` and `heading`
  - `components`: the persisted Lua components (JSON, see "Lua components and systems")
  - `created_at`, `last_login_at` and `deleted_at`
- **Timestamps:** `DATETIME(3)` in UTC, written by the application.
- **Queries:** every query is a prepared statement scoped by `realm_id`. Ownership checks use `account_id`.
- **Deleting:** a delete is a soft delete. The name stays reserved, because the unique key `(realm_id, name)` with `ascii_general_ci` covers deleted rows too, so the name can't be reused.

### Save and load rules

The world never touches MySQL from the game thread. One DB worker thread owns the connection. Requests come in on a queue, and results go back to the game thread through `asio::post`.

- **When saves happen:** on logout or disconnect, on a duplicate-login kick, on graceful shutdown, and every `SAVE_INTERVAL_MS` while the position or a persisted Lua component is dirty.
- **Never dropped:**
  - A save sits in a pending map keyed by character, where the latest save wins.
  - It is retried with exponential backoff (`SAVE_RETRY_MIN_MS` up to `SAVE_RETRY_MAX_MS`) until it lands.
  - Every failed attempt is logged (`save failed, will retry`).
- **Ordering:** saves run before queued requests. `EnterWorld` first lands any pending save for that character, so a load never reads a stale position.
  - **Duplicate login:** the kicked session's save is queued before the new session can ask for anything.

### When MySQL goes down at runtime

| | Behaviour |
|---|---|
| New auth handshakes | Still work: they only need the JWKS |
| Character list/create/delete/enter | `CharError{service_unavailable}`, within `DB_CONNECT_TIMEOUT_SECONDS` at worst. After a failed connect, the worker answers immediately for 1 s instead of reconnecting |
| Players in the world | Keep moving, chatting and replicating, because the tick never waits on the DB |
| Saves | Retried with backoff. They land after MySQL comes back. `tick stats` logs show `saves_pending` and `saves_failed_attempts` |

### Graceful shutdown

SIGTERM, SIGINT or Ctrl+C starts the shutdown:
1. The server stops accepting connections and logs `tick stats total`.
2. Lua: `onLeave` runs for every character, then every system's `shutdown`.
3. Every character in the world is despawned and saved, and every client gets `Disconnect{server_shutdown}`. Then every NPC is removed, with nothing sent or saved.
4. Once the clients are gone, the DB worker flushes the pending saves, ignoring backoff, until `SHUTDOWN_TIMEOUT_MS - 500 ms`.
5. The process exits 0, or 1 if any save could not be flushed.

Compose gives the container a `stop_grace_period` of 15 s.

## Lua components and systems

Game developers define data components in Lua and write server-authoritative Lua systems that run on the 20 Hz tick
and operate on player characters. The component file format is the engine's (root README, "Lua components"), so a
game project's `components/` folder can be shared by the client and the server. The engine applies whatever the
server replicates to the matching component on the local player and on remote clones (root README, "Online Services").

### Layout

| Path | What |
|---|---|
| `LUA_ROOT/components/**/*.lua` | Component definitions, scanned recursively, one type per file |
| `LUA_ROOT/fsms/**/*.lua` | State machine definitions, scanned recursively, one machine per file |
| `LUA_ROOT/systems/*.lua` | Systems, not recursive, run in alphabetical filename order |

- **`LUA_ROOT`:** defaults to `/app/lua`. The image ships the sample from `services/world/lua/` there, and compose bind-mounts `./lua` over it.
- **Loading:** files are loaded once at startup; there is no hot reload. After an edit, restart the server (`docker compose restart world`).
- **Errors refuse the start:** an invalid component or state machine definition, an invalid system file, a `GAME_STATE_MACHINES` name with no valid definition, or an error in a system's `init` stops the start. The server logs every error, not just the first, one `Lua error` line each. Each starts with the file path (and `path:line` for Lua errors); a `startup failed: Lua` line and exit status 1 follow.
- **No Lua is fine:** a missing or empty `LUA_ROOT` means no custom components, state machines or systems, with an info log.

**Sharing with a game project:** point the bind mount (or `LUA_ROOT`, when running on the host) at a folder whose `components/` and `fsms/` are the game project's. For example, change the volume to `- ../../my-game:/app/lua:ro`, which holds `components/`, `fsms/` and a `systems/` folder the client never runs. Client and server then read the same definition files.

### Component definitions

Exactly the engine's format: `return { name, displayName?, fields = {...} }`.
- **Field types:** `float`, `int`, `bool`, `string`, `vec3`, `quat`, `enum`, and `array` with `of`.
- **Field options:** `min`, `max`, `color`, `values`, `default`.
- **Checks:** the same strict key checks and error messages as the engine.

There are two more top-level keys. The engine accepts and checks both, but uses neither:

| Key | Values | Default | Meaning |
|---|---|---|---|
| `persist` | `true` / `false` | `false` | Saved per character in MySQL (see Persistence below) |
| `replicate` | `"none"` / `"owner"` / `"aoi"` | `"none"` | `owner`: sent to the owning player only. `aoi`: to the owner and every player who can see the character. `none`: never sent |

```lua
-- lua/components/health.lua (the sample)
return {
    name = "Health",
    persist = true,
    replicate = "aoi",
    fields = {
        { name = "current", type = "float", default = 100, min = 0 },
        { name = "max",     type = "float", default = 100, min = 1 },
        -- ...
    },
}
```

- **Invalid values:** a bad `persist` or `replicate` is a definition error.
- **Reserved names:** a definition may not be named `Character`, `Npc`, `Transform` or `StateMachine` (the built-ins), or after any server C++ component (`Viewers`, `SessionLink`, `CharacterRef`, `MoveState`, `PendingMoves`, `PendingSay`, `ChatLimit`, `AoiCell`, `Visible`, `Moved`, `Dirty`, `SaveTimer`, `ReplicatedState`, `ScriptsLeft`, `PreservedComponents`, `ComponentData`, `StateMachineRuntime`).
- **Duplicates:** two files declaring the same name are both rejected, each error naming the other file, whatever the scan order.

### Systems

A system file returns a table; only `update` is required:

```lua
return {
    init = function() end,         -- at startup, after the file loaded; an error here refuses the start
    update = function(dt) end,     -- every tick; dt = 1 / TICK_HZ seconds
    shutdown = function() end,     -- graceful shutdown, before the final saves
    onEnter = function(e) end,     -- a character spawned; its persisted components are already restored (never an NPC)
    onLeave = function(e) end,     -- a character is leaving; changes made here are saved (never an NPC)
}
```

- **One state, isolated files:** the server has one Lua state, and each system file gets its own environment, so globals never leak between files.
- **Sandbox:** the same as the engine's. Only base, table, string and math are open, with no `io`, `os`, `ffi`, `debug`, `jit`, `package`, `require`, `loadfile`, `dofile`, `load` or `loadstring`. Definition files are sandboxed too.
- **Runaway scripts:** a callback that runs longer than 2 s raises an error, so an infinite loop disables its system instead of hanging the server.
- **Runtime errors:** an error in any callback is logged as `Lua system disabled`, with the system, the callback and `path:line`. That system is then disabled until restart; the server and the other systems keep running.

**The `kast.*` API** has the engine's semantics and error messages, so a script behaves the same on client and server:

| Function | What |
|---|---|
| `kast.query(names...)` | Iterator over entity ids with every listed component, in ascending id order. Captured when called: entities gone later in the loop are skipped |
| `kast.get(e, name)` | A fresh table of the component's fields; arrays are 1-based sequences, `vec3`/`quat` are `{x=, y=, z=[, w=]}` |
| `kast.set(e, name, fields)` | Partial: listed fields change, the others keep their values. All-or-nothing: an unknown field, a wrong type, an out-of-range value or an unknown enum value raises an error and leaves the component unchanged |
| `kast.has(e, name)` / `kast.add(e, name)` / `kast.remove(e, name)` | `add` uses the field defaults and does nothing if the component is present; `remove` does nothing if it is absent |
| `kast.create(name, opts)` | Creates an NPC and returns its entity id; `opts = {template = string, map = id?, position = {x, y, z}?, heading = number?}` (see [NPCs](#npcs)) |
| `kast.transfer(e, map, position [, heading])` | Moves player or NPC `e` to `position` (`{x, y, z}`) on map `map`; `heading` defaults to the current one. Returns `true`, or `false` if this process does not own the destination (see [Maps](#maps)) |
| `kast.destroy(e)` | Removes NPC `e`; the players who see it get `Despawn` |
| `kast.log/warn/error(msg)` | A server log line `<system path>: <msg>` at that level |
| `kast.publish(name [, payload])` | Queues an event on the server's event bus for the next drain (see [Events](#events)) |
| `kast.subscribe(name, fn)` | Calls `fn(payload)` for every later `name` event; returns a handle |
| `kast.unsubscribe(handle)` | Removes the calling system's subscription; another system's or a stale handle does nothing |
| `kast.sendTo(e, name [, payload])` | Sends the event to character `e`'s client only (an NPC has none: nothing is sent) |
| `kast.sendNearby(e, name [, payload])` | Sends the event to `e`'s client and every client that currently sees `e` (the `replicate = "aoi"` audience) |
| `kast.broadcast(name [, payload])` | Sends the event to every player in the world, on every map |
| `kast.fsm.state/change/canChange/states(target [, state])` | Game and character state machines (see [State machines](#state-machines)) |
| `kast.map.area(e)` / `kast.map.areaAt(map, x, y, z)` | The area `e` or a point is in: `{id, name, flags, music}` or `nil` (see [Areas](#areas)) |
| `kast.map.ground(map, x, z)` | The ground's height at a point on loaded map `map`, or `nil` off its tiles (see [Ground](#ground)). Raises on an unloaded map |
| `kast.map.path(map, from, to)` | The navmesh path between two `{x, y, z}` points: a list of `{x, y, z}`, or `nil` (see [Navmesh](#navmesh)). Raises on an unloaded map or a bad point |

- **Entities:** an entity is a player character's or an NPC's server wire id, the integer clients see as `entityId`. `kast.query("Character")` finds the players, `kast.query("Npc")` the NPCs, and `kast.query()` both.
- **`kast` is read-only:** assigning to it raises an error.
- **Read-only built-ins:** every character has `Character` and `Transform`, every NPC `Npc` and `Transform`, and an NPC from a spawn file also `Spawn`. `get`, `has` and `query` work on them; `set` and `add` raise `"<name>" is read-only on the server`, and `remove` raises `"<name>" cannot be removed`. The one exception is `set` on an NPC's `Transform` (see [NPCs](#npcs)).
- **Writable built-in:** `StateMachine` (`machine`, `initialState`, both strings) works with every function, like a Lua component that is never persisted or replicated (see [State machines](#state-machines)).

| Name | Fields |
|---|---|
| `Character` | `charId`, `name` (players only) |
| `Npc` | `name`, `template` (NPCs only) |
| `Spawn` | `id`, and with a path `path = {{x, y, z}, ...}`, `wait` (seconds), `loop` (NPCs from a spawn file only; see [NPC spawns](#npc-spawns)) |
| `Transform` | `position = {x, y, z}`, `heading` (radians), `map` (id, read-only). A player moves only by the validated client movement, an NPC only by `kast.set`; either changes map only by `kast.transfer` |
| `StateMachine` | `machine`, `initialState` (writable; added only by `kast.add`) |

### NPCs

A system creates server-authoritative NPCs with `kast.create`, moves them by setting their `Transform`, and removes them
with `kast.destroy`. Clients see them through the usual interest management: a player in range gets `Spawn` with
`kind` npc and the NPC's `template` ([`PROTOCOL.md`](PROTOCOL.md#interest-management)), then `EntityMove` as it moves,
and `Despawn` when it leaves the view or is destroyed. The engine shows each one as a clone of the scene entity named
after its template (root README, "Replication").

```lua
local guard = kast.create("Guard", {template = "Guard", position = {x = 1034, y = 0, z = 1024}, heading = 0})
kast.set(guard, "Transform", {position = {x = 1035, y = 0, z = 1024}}) -- one EntityMove to every player who sees it
kast.destroy(guard)                                                 -- Despawn to every player who sees it
```

- **`kast.create(name, opts)`:** `name` and `opts.template` must be non-empty strings of at most 255 bytes.
  `map` defaults to `START_MAP` and must be a loaded map's id. `position` (`{x, y, z}`, finite numbers) defaults to that
  map's spawn point and `heading` (radians) to 0. A position off the map is clamped to the nearest point on a declared
  tile, and y is the [ground](#ground) there (no flying NPCs). Any other key in `opts`, a bad value, or `MAX_NPCS` NPCs
  already in the world raises an error at the call (`kast.create: template must be a non-empty string of at most 255
  bytes, got nil`) and creates nothing. It works from `init` on; players in range get the `Spawn` on the next tick.
- **`kast.destroy(e)`:** a player id (`kast.destroy: entity 7 is a player; only NPCs can be destroyed`), a stale id or
  an invalid id raises an error. A destroyed NPC's id is stale at once: `kast.*` on it raises `invalid entity id`.
- **Moving:** `kast.set(npc, "Transform", {position = {x, y, z}, heading = h})` is partial and all-or-nothing like any
  `set`, and clamps onto the NPC's map like `create`. `map` cannot be set (`"Transform": "map" is read-only; use
  kast.transfer`). Script moves are trusted: there is no speed check. A move goes out as `EntityMove`
  in the same tick, like an accepted player move. A move into or out of a player's view gives `Spawn` / `Despawn` on
  the next tick.
- **Components and machines:** `kast.add/set/get/has/remove`, and `StateMachine` with `kast.fsm.*`, work on NPCs as on
  characters. An `aoi` component reaches the players who see the NPC, an `owner` one nobody, and `persist` is ignored.
- **Events:** `kast.sendTo(npc, ...)` sends nothing (no error); `kast.sendNearby(npc, ...)` reaches the players who see
  it. `onEnter` and `onLeave` never run for NPCs.
- **Ephemeral:** NPCs are never saved and need no database. A restart starts without them: the spawn files and the
  systems' `init` create them again. A graceful shutdown removes them silently, after the players.

**NPCs cost bandwidth like players.** Each move is one `EntityMove` per tick to each player in view, so a crowd of
walking NPCs around a crowd of players costs NPCs x players messages a tick. Move NPCs at the rate clients need and let
the engine interpolate.

### NPC spawns

A map's NPCs are data: one file per tile, `<MAP_ROOT>/<folder>/.server/spawns/<tx>_<tz>.json`. The dot-folder keeps it
server only (the editor's Build leaves dot-folders out of `game.pak`).

```json
{ "version": 1,
  "paths": {"gate_patrol": {"loop": true, "wait": 2,
            "points": [[1034, 0, 1034], [1034, 0, 1014], [1014, 0, 1014], [1014, 0, 1034]]}},
  "spawns": [
    {"id": "guard_1", "name": "Guard", "template": "Guard", "pos": [1024, 0, 1024], "heading": 0,
     "respawnSec": 30, "path": "gate_patrol"}
  ]}
```

- **Spawn:** `id` (non-empty, unique on the map), `template` (the clone clients show, as in `kast.create`), `pos` and
  `respawnSec` are required. `name` defaults to `template` (both at most 255 bytes), `heading` (radians) to 0. `pos` must
  lie inside the file's tile; its y is replaced by the [ground](#ground) there, like `kast.create`.
- **Respawn:** `respawnSec` (0 to 31536000, a year) is how long after an NPC is destroyed (`kast.destroy`) the spawn
  places a new one, at `pos` again, as a new entity id. `-1` never respawns.
- **Paths:** named in the file's `paths` and used by its spawns. `points` (at least one `[x, y, z]`) is the route,
  `wait` (seconds, default 0) the pause at each point, and `loop` (default `true`) goes from the last point back to the
  first; `false` walks back along the route. The server does not move NPCs: a Lua system reads the path from the
  read-only `Spawn` component (`kast.query("Spawn")`, `kast.get(e, "Spawn")`). The sample `systems/npcs.lua` walks
  every NPC that has one, along `kast.map.path` between its points on a map with a [navmesh](#navmesh).
- **Loading:** at startup, the spawn files of every tile this process owns (every tile, for now). A file name that is
  not `<tx>_<tz>.json`, a tile not on the map, an invalid file (each problem named, such as
  `maps/start/.server/spawns/4_4.json: spawn "guard_1": "pos" is outside tile 4_4`), an unknown `path` or an `id` used
  twice on a map refuses the start. `map::parse_spawns` (the `kast_map` library) checks one file without the server.
- **Spawning:** every spawn is placed on the first tick, and each tick the spawner places the ones due, before the Lua
  `update`, so systems see them that tick; players get the `Spawn` on the next. They count toward `MAX_NPCS`: at the
  cap the due spawns wait (one warning, `spawns deferred: MAX_NPCS reached`) and go in once there is room. Every spawn
  is always active, near players or not.

### Events

The server has one event bus with the engine's API and rules (root README, "Events"). Systems use it to talk to each
other, and server C++ can publish and subscribe through `World::bus()`. Events never reach clients on their own: only
the three send functions put anything on the network.

- **Delivery:** `kast.publish` only queues. The queue is drained once per tick, right after every system's `update`
  (step 6 below). Events are delivered in publish order, and each event's subscribers run in subscription order, C++
  and Lua alike. Events published during the drain (by a subscriber), or from `init`, `onEnter` or `onLeave`, wait for
  the next tick's drain. A graceful shutdown drops the queue.
- **Payloads:** plain data. That is `nil`, booleans, finite numbers, strings, and tables whose keys are exactly 1..n
  (arrays) or all strings (objects), nested at most 32 deep. Functions, userdata, threads, cycles, mixed keys, NaN and
  infinities raise an error at the call (`kast.publish: payload.x must be a finite number`), and nothing is queued.
  Every subscriber gets its own copy.
- **Names:** non-empty strings. Anything else raises `<function>: event name must be a non-empty string, got <value>`.
- **Subscriptions belong to the calling system.** When that system is disabled by any error, all of its subscriptions
  are removed. A failing subscriber is logged as `Lua system disabled` (callback `event '<name>'`, with `path:line`),
  and the event's other subscribers still run.
- **Client events:** `kast.sendTo`, `kast.sendNearby` and `kast.broadcast` take the same names and payloads. Each
  recipient gets a `ServerEvent` ([`PROTOCOL.md`](PROTOCOL.md#server-events)), and the engine publishes it on the
  client's bus as `server.<name>`.
  - **When:** sends are queued and go out in call order after component replication (step 8), so a client already has
    the `Spawn` and the latest component values. Sends from `onLeave` go out at the despawn, before that character's
    `Despawn`; sends from `onEnter` go out on the next tick.
  - **Targets:** a bad id (not a whole number from 0 to 2^32-1) raises `kast.sendTo: invalid entity id <value>`. A
    valid id whose character or NPC is not in the world when the events go out (it left meanwhile) sends nothing.
  - **Size:** an event whose frame would pass 65535 bytes raises `kast.sendTo: event "<name>" is too large to send` at
    the call.

**Client events cost bandwidth.** Each send is one message per recipient, and nothing rate-limits them: only the
existing 1 MiB per-session send-buffer cap stops a flood, by disconnecting the client. `broadcast` reaches every
player, so one call per player costs N² messages in a zone of N. `sendNearby` in a crowd costs about the same. Send on
game events (a login, a kill, a quest step), never every tick.

### State machines

Server Lua and server C++ can drive finite state machines with the engine's definition files and rules (root README,
"State machines"). That section is the spec for the file format, validation and its messages, `ctx`, the transition
rules, and the `kast.fsm` API. A game project's `fsms/` folder can be shared with the server like `components/`. The
port lives in `server/lua/state_machines.*` and `server/lua/lua_state_machines.*`.

- **Definitions:** `LUA_ROOT/fsms/**/*.lua`, one machine per file, loaded once at startup before the systems' `init`.
  Each file runs in its own sandboxed environment with the full read-only `kast` API. `kast.log` from a callback is
  prefixed with the definition path (`fsms/door.lua: ...`). A missing `fsms/` folder means no machines.
- **Game machines:** `GAME_STATE_MACHINES` lists them by name. After every system's `init`, each enters its `initial`
  state: `enter` runs with `ctx.from = nil`, and no `fsm.changed` is published. A machine that `kast.fsm.*` touches during
  an `init` enters right then, once. Game machines run for the server's lifetime; shutdown runs no `exit`.
- **Character and NPC machines:** use the writable built-in `StateMachine` (`machine`, `initialState`) through `kast.add`,
  `kast.set`, `kast.get`, `kast.has`, `kast.remove` and `kast.query`.
  - `kast.add` alone leaves `machine` empty: there is no machine yet, and `kast.fsm.*` on that character raises.
  - Setting `machine` enters that machine's initial state at once. That is `initialState` if it names a state,
    otherwise the definition's `initial`, with a warning. `from` is `nil` and no event is published.
  - Changing `machine` enters the new machine's initial state, with no `exit` of the old one. Changing only
    `initialState` does nothing. A `machine` with no valid definition is logged once.
  - Removal, logout and graceful shutdown just drop the machine, with no `exit`.
  - The typical use is a system's `onEnter` adding and setting it (see the sample).
- **Not persisted, not replicated:** `StateMachine` and the current state never reach MySQL (the `components` column)
  or clients. A character starts its machine again from the initial state on every login. To tell clients about a
  transition, send it from `enter` (`kast.sendTo` / `sendNearby`).
- **Events:** `fsm.changed` `{machine, entity, from, to}` goes on the server's event bus, with `entity` `null` for game
  machines and the publisher set to the definition path. It is delivered at the normal drain (see [Events](#events)), so
  a transition made in `update` reaches subscribers in the same tick.
- **Callback errors:** an error in `enter` or `exit` is logged as `State machine fsms/door.lua: enter "open" failed:
  fsms/door.lua:5: ...` with a traceback. The transition still commits and the machine stays usable; no system is
  disabled. A callback runs under the 2 s script budget of whatever entered Lua. When a system's `kast.fsm.change`
  triggers a runaway callback, the callback is stopped and logged, and then that system is disabled too, because they
  share one budget (as in the engine).
- **Misuse:** every bad `kast.fsm.*` argument raises at the calling line, which a system callback treats like any other
  error (the system is disabled). An unknown state name passed to `change` is a denial (`false` and a warning).
- **Startup:** a rejected `fsms/` file or a `GAME_STATE_MACHINES` name with no valid definition refuses the start, like
  any other Lua error (one `Lua error` line each, `startup failed: Lua`, exit status 1). That also applies when
  `LUA_ROOT` is missing and `GAME_STATE_MACHINES` is set.
- **C++:** `World::fsm()` (`lua::StateMachines`, also `Runtime::fsm()`) gives `state`, `change`, `canChange` and `states`
  for a game machine name or a character or NPC wire id. It applies the same rules, events and logging as `kast.fsm`, and a bad
  target throws `std::invalid_argument`.
- **Not on the server:** hot reload, scenes and `kast.loadScene`, the editor's
  panels.

```cpp
lua::StateMachines& fsm = world.fsm();
std::string phase = fsm.state("zone_cycle");          // a game machine by name
bool moved = fsm.change(lua::EntityId{7}, "moving");  // a character's machine by wire id
```

### Tick order

Each tick runs, in this order:

1. movement
2. **areas:** `area.leave` / `area.enter` for players that moved, and portal transfers ([Areas](#areas))
3. AOI (`Spawn` / `Despawn`)
4. chat
5. **spawner:** the [NPC spawns](#npc-spawns) that are due
6. **Lua `update`**, every system in filename order
7. **event drain:** the events queued since the last drain go to their subscribers
8. replication (`EntityMove`)
9. component replication
10. **client events:** `ServerEvent` for every `kast.sendTo` / `sendNearby` / `broadcast` since the last flush
11. save
12. end of tick

Lua sees this tick's validated positions. Its changes are replicated and become eligible for saving in the same tick.
An NPC that Lua creates, or moves into or out of a player's view, gets its `Spawn` / `Despawn` at the next tick's step 3.

Lifecycle hooks run at these points:

| Event | Hooks |
|---|---|
| `EnterWorld` | Spawn, restore the persisted components, then `onEnter` |
| Logout, disconnect or duplicate-login kick | `onLeave` and its client events, then the save and the despawn |
| Graceful shutdown | `onLeave` for every character, then every system's `shutdown`, then their client events, then the saves and despawns. Undelivered bus events are dropped |

### Persistence

- **Storage:** components with `persist = true` are stored in `characters.components`, a JSON column added by `migrations/002_character_components.sql`. It holds `{"<Name>": {...}, ...}` in the replication encoding.
- **When they are saved:** with the position, through the existing save flow on the DB worker. That means on logout/disconnect and on graceful shutdown. Every `kast.set` / `add` / `remove` that changes a persisted component marks the character dirty, so the `SAVE_INTERVAL_MS` save picks it up. The tick never waits on MySQL.
- **Loading (drift rules, as in the engine):** a missing field gets its default, an unknown field is dropped, and a wrong-typed or out-of-range value is reset to its default. Each fix logs a `persisted component fixed on load` warning naming the character and field.
- **No data loss:** column entries with no current definition, or whose definition is no longer `persist`, are kept verbatim and written back unchanged on every save. A column that isn't a JSON object is never overwritten (and nothing is restored from it).

### Replication

`ComponentUpdate` and `ComponentRemove` are described in [`PROTOCOL.md`](PROTOCOL.md#lua-components).

- **What is sent:** after the Lua systems run, every `owner` / `aoi` component that was added, changed or removed since it was last sent. The diff is per component against the last sent JSON, so unchanged components are never resent.
- **Full sets:** the owner gets its full set on the first tick in the world. A viewer gets an entity's full `aoi` set when the entity comes into view, right after its `Spawn`.
- **NPCs:** the same rules without an owner: `aoi` components go to the players who see the NPC, `owner` ones to nobody.
- **Ordering:** data never precedes `Spawn` or follows `Despawn`.
- **Client writes:** there are none. A client that sends a component message is disconnected with `unexpected_message`.

### Sample

The `lua/` folder holds the sample:

| File | What |
|---|---|
| `components/health.lua` | `Health`: `persist`, `replicate = "aoi"`, every field type. The same file as `tests/project/components/health.lua` |
| `components/wallet.lua` | `Wallet { gold }`: `persist`, `replicate = "owner"` |
| `fsms/locomotion.lua` | Character machine `idle` / `moving`, each enterable only from the other. Every `enter` sends the owner `fsm.state` `{machine, from, to}` (`kast.sendTo`) |
| `fsms/zone_cycle.lua` | Game machine `day` / `night` (listed in `GAME_STATE_MACHINES`). `enter` only logs |
| `fsms/patrol.lua` | NPC machine `walking` / `waiting`, each enterable only from the other. No callbacks: it only records the phase |
| `systems/locomotion.lua` | `onEnter` adds `StateMachine` with `locomotion`, which enters `idle`, then tries `idle` again: denied by `allowedFrom`, a warning and no event. `update` switches to `moving` when the character's `Transform` changes and back to `idle` after 1 s standing still. It flips `zone_cycle` every 5 minutes if it is listed. It sends only on transitions, never per tick |
| `systems/events.lua` | On login, sends the player `welcome` (`sendTo`) and publishes `player.entered` on the bus. Its subscriber runs on the next tick, after the newcomer's `Spawn`, and sends `player.arrived` to the newcomer and everyone who sees it (`sendNearby`). On logout, sends every player `player.left` (`broadcast`) before the `Despawn`. It sends only on login and logout, never per tick. It also subscribes to `area.enter` / `area.leave` and logs each (`area.enter entity 5 map 1 area goldshire (Goldshire) flags {noMount=false, pvp=true}`) |
| `systems/npcs.lua` | Patrols every NPC whose `Spawn` has a path: the first time it sees one it adds `StateMachine` with `patrol`, then walks it at 3 m/s to each point in turn, facing where it goes, and waits the path's `wait` at each (`loop`, or back along the path). While walking it costs one `EntityMove` per tick per viewer. No NPCs or waypoints of its own: the sample maps' spawn files place the "Guard" (template `Guard`, `tests/project/maps/start/.server/spawns/4_4.json`: respawn 5 s, a 20 m square around the spawn point, 2 s waits) and a still "Bat" (template `Guard`, never respawns) at map 2's spawn point, the far side of the sample portal |
| `systems/test_hooks.lua` | Test only, for scenario 18: a character whose name starts with `Respawntest` destroys the Guard (spawn `guard_1`) when it enters. Remove it from a real game |
| `systems/regen.lua` | `onEnter` adds `Health` and `Wallet` if absent and pays 1 gold per login. Once a second per character, each on its own phase, `update` regenerates `Health.current` toward `max` (+2) if the character stood still, or drains it by 0.5 per metre moved (`Transform` deltas). It keeps whole points and writes only on a change, so a character costs at most one `ComponentUpdate` a second per viewer |

**Keep replicated components quiet.** Each change goes out in full to the owner and everyone in view. A component written every tick costs one message per tick per viewer, and in a crowd that is N² messages. An earlier sample that updated `Health` every tick made the 500-bot load test drop 378 clients on the 1 MiB send-buffer cap while they crowded the spawn point. Write replicated values at the rate clients need, and only when they change.

## Operations

- **Logs:** JSON lines on stdout (`ts`, `level`, `msg`, fields).
- **Tick stats:** every `TICK_STATS_INTERVAL_SECONDS`, the server logs a `tick stats` line, and a `tick stats total` line on shutdown. They contain:
  - `p50_ms`, `p99_ms` and `max_ms`: systems time per tick.
  - `lag_p99_ms` and `lag_max_ms`: how late the tick timer fired.
  - `population` and `connections`.
  - `saves_ok`, `saves_failed_attempts` and `saves_pending`.

## Tests

- **Self-check** (`tests/self_check.cpp` + `tests/lua_checks.cpp`, ctest `world_self_check`): assert-style checks of the pure logic with an injected clock, so nothing sleeps. The Lua checks write their scripts to a temp folder and drive a `World` with captured sends, saves and log lines. They cover the event bus (the engine's cases), `kast.publish/subscribe/unsubscribe` and the client sends: audiences, order, rejections and timing. They also cover state machines, from definitions to C++ / Lua parity.
- **Maps** (in `lua_checks.cpp`): tile math at the edges, every manifest and `Maps::load` error, bounds `Correction`,
  per-map AOI, `World::transfer` message order, map save/load fallbacks, and `kast.transfer` / `Transform.map`.
- **Areas** (in `lua_checks.cpp`): rect, concave polygon, `y` range, priority and parent flag inheritance; every
  `areas.json` and portal load error; the sample maps loading. With the sample maps and `lua/`: `area.enter` on login
  (and `events.lua`'s log line), `area.leave` then `area.enter` exactly once on crossing a border, and a walk into the
  cave portal (map, position, heading, events, `MapChange` before `Despawn` before `Spawn` in that tick, no
  `EntityMove` leaking to the old neighbours, the save on map 2). `kast.map.area` / `areaAt` for players, NPCs and
  points, and their errors.
- **NPC spawns** (in `lua_checks.cpp`): `parse_spawns` with every field, default and rejection; every spawn file
  load error (file name, tile not on the map, `pos` outside the tile, duplicate id across a map's files); respawn on the
  injected clock (not back at 29.95 s, back at 30 s at its `pos`, `-1` never), `MAX_NPCS` deferring with one warning;
  the `Spawn` built-in from Lua (`query`, fields, read-only). With the sample maps and `lua/`: the Guard and the Bat
  from their files, the Guard visiting its 4 path points at 3 m/s, and the test hook's destroy and 5 s respawn.
- **Ground** (`self_check.cpp` movement + `lua_checks.cpp`): `validate_move` lifting under-ground moves, `in_air` for
  rising or hovering past `MOVE_MAX_AIR` (spending no budget), falling from 50 m; the two env vars. A generated
  ramp map loaded from `.r16` files: `Maps::ground` at samples, between them, on tile edges, a tile without a file,
  off the map, and equal to `map::ground`; a bad heights file. With a `World` on it: entering under the ground,
  moves (lifted, `Correction`, falling), an NPC created and a spawn file at y = 100 standing on the ground, a transfer
  landing on it, and `kast.map.ground`. With the sample maps: hills' PNG heightmaps load.
- **Navmesh** (`lua_checks.cpp` + ctest `mapbake_cli`): an in-memory bake of a 2-tile ramp with a 22 m block whose
  sides are 70 degree slopes: `seam_errors` on a torn edge, a path from tile 0_0 to tile 1_0 that goes round the block,
  no path to a point off the mesh, `on_mesh` false on the 70 degree slope and 20 m up, true on the ramp, the seam and
  5 m up. Navtiles with a bad magic, navtile or Detour version, another tile's blob, or truncated: each a load error.
  Moves: onto the slope `Correction`, along the ramp accepted, `MOVE_NAV_CHECK=0` accepted, a map without navtiles
  as in Ground; `validate_move`'s `off_mesh` spends no budget. `kast.map.path` from Lua (points, ends, the detour,
  `nil` cases, errors). With the sample maps: hills on the mesh on a 32 degree slope (the client's 40 degree limit, not
  Recast's ledge filter's ~27) and off it on the mesa wall, and the Sentry walking round the mesa on the ground.
  `mapbake_cli` runs the tool: a torn seam and a spawn outside its tile exit 1 with nothing baked, and (in the repo)
  hills' stored navtiles equal a fresh bake.
- **Integration** (`tests/integration.sh`): drives `kast_bot` scenarios against the compose stack. Scenario 10a
  (`persist`) also checks that a new character enters map 1 and keeps its map across a relog.
  - The bots register, verify through Mailpit and log in with real auth tokens.
  - Scenarios 10–13 use `docker compose restart/stop/start` on `world` and `mysql`.
  - Scenario 14 uses the sample `systems/events.lua`: `sendTo` reaches only its target, `sendNearby` only bots in view, and `broadcast` every bot.
  - Scenario 15 (`fsm`) uses the sample `locomotion` machine. On entering, a bot gets exactly one `fsm.state` (`nil->idle`), because the denied `idle->idle` sends nothing. Walking, then standing still, gives `idle->moving` and then `moving->idle`. A bot in view gets none of them.
  - Scenario 16 (`npcs`) uses the sample `systems/npcs.lua` and the Guard's spawn file. A bot entering at the spawn point gets `Spawn` for the Guard (`kind` npc, `template` `Guard`) and `EntityMove`s at changing positions. Walking 200 m away gives `Despawn` and then nothing about it; walking back gives `Spawn` again.
  - Scenario 17 (`portal`) uses the sample `areas.json` files. Bot A walks from the spawn point into `cave_mouth`.
    It gets `MapChange{2}` at the portal destination before any `Despawn`/`Spawn`, then sees the Bat and loses bot B,
    who gets A's `Despawn`. After a relog A is still on map 2.
  - Scenario 18 (`respawn`) uses the sample `systems/test_hooks.lua`. Bot A sees the Guard; bot B enters as
    `Respawntest...`, and A gets the Guard's `Despawn`, then 4.5–6.5 s later a `Spawn` for a new Guard (new id, template
    `Guard`) at its spawn point.
  - Scenario 0 runs `kast_mapbake` in the image on the mounted hills (into `/tmp`; the maps are read-only) and logs
    the fresh and stored navtiles' checksums.
  - Scenario 19 (`terrain`) reads `maps/hills` itself (`--map-root`, the maps mounted into the bot). Bots A and B take
    `hills_gate` to map 3 and land on the ground. A walks 40 m down the slope (9 m, at most 36.5 degrees, under the
    navmesh's 40) at the ground's height with no `Correction`, and B's `EntityMove`s for A are within 5 cm of the
    ground. A rising 20 m then 30 m above the ground gets `Correction` back to its last position.
  - Scenario 20 (`navmesh`) takes bot A to the hills: walking at ground height to the mesa's south foot gets no
    `Correction`; walking on up its wall does. Over 30 s, the Sentry's `EntityMove`s pass the mesa's x range, all on
    the ground and at most 0.5 m into its walls' foot (a navmesh corner may graze it).
  - Logs go to `build/integration-<run>/`.
- **Load** (`tests/load.sh`):
  - **Setup:** restarts `world` so its totals cover only the test. It then runs `COUNT` bots in one process (8 io threads, 50 new connections per second) for `DURATION` s.
  - **Pass criteria:**
    - Bot summary PASS: every bot entered the world, with no errors, no unexpected disconnects and no `Correction` for honest movers.
      A wanderer that walks into the sample portal stops on the other map (`map_changes_recv`); the corrections for
      its moves still in flight are not counted.
    - Every 10 s window's tick p99 is under 25 ms, and so is the run total.
    - Zero failed save attempts.

## Deliberate simplifications

Each is marked `// ponytail:` in the code with its limit and upgrade path:
- **One database writer per realm:** character limits are checked with count-then-insert on the single DB thread. Several world processes per realm would need `SELECT … FOR UPDATE`.
- **Migration splitter:** splits on `;` at the end of a line.
- **Component replication diff:** every replicated component's full JSON is compared with the last sent value each tick, and a change is sent in full. Fine for hundreds of players with a few small components. Past that, use dirty flags set by `kast.set`, and delta or binary encoding.
- **`kast.set` through JSON:** reuses the definition checks; convert Lua values directly if it shows in profiles.
- **No TLS `close_notify` on disconnect:** peers see EOF.
- **Map bounds by rectangle scan:** each bounds check walks a map's tile rectangles. Fine for a handful per map; use a
  tile set if maps grow many.
- **Area overlap check:** an exact test, except that two non-convex polygons with exactly coincident edges and outside
  centroids can slip through. It compares every pair of areas at load (O(n^2)), which is fine for hundreds per map.
- **Load bots park after a portal:** they stop moving on the destination map instead of wandering its bounds.
- **No jump or flying allowance:** `MOVE_MAX_AIR` is one limit for everyone, and NPCs are always on the ground. A
  jump arc or flying mounts need a per-entity allowance, and flying NPCs a flag.
- **Off the navmesh is corrected at once:** a player cannot fall off a cliff face. A grace period (or an allowance
  while falling) if drops are wanted.
- **Navmesh cell height:** fixed at `cellSize / 2`; a manifest field if maps need finer vertical steps.
- **32-bit poly refs:** about 4096 polygons per tile at 1024 tiles a map; `DT_POLYREF64` past that.
- **One process owns every tile:** `Maps::owns` is always true for a loaded tile. Several processes need a refused
  transfer to save and send `Redirect{host, port, ticket}` to the owner.
