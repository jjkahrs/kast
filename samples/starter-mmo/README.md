# Starter MMO

A small but complete Kast online game, made to read and copy. You open it in the editor, then:

1. register and verify an account through the in-game GUI;
2. log in and pick a realm;
3. create or pick a character;
4. walk the Meadow with other players and a patrolling Guard, chat, and watch the server's Health and Gold in the HUD;
5. take the portal into the Cavern, where a Bat waits, and walk back.

The same folder is the game project (client) **and** the world server's Lua and maps: client and server share
`components/`.

## What is where

| Path | Side | What |
|---|---|---|
| `project.kast` | client | `Starter MMO`, starts at `scenes/login.scene`, Online Services on (`http://localhost:3100`, CA `certs/world_dev_ca.pem`), game state machine `session` |
| `scenes/` | client | `login`, `register`, `realms` (one screen each) and `world` (character select, then the game) |
| `scripts/` | client | one Lua system per screen or concern; they talk through the event bus |
| `ui/` | client | RmlUi documents, one per screen, sharing `starter.rcss` (the Kast editor palette) |
| `components/` | **both** | `Health` (replicated to everyone in view), `Wallet` (owner only), `MapProps` (client-side tag) |
| `fsms/` | both | `patrol` (the Guard's machine, server), `session` (why the client is on the login screen) |
| `npcs/` | server | the `Guard` and `Bat` NPC definitions; their `template` names the client entity they are shown as |
| `systems/` | server | `regen.lua` (Health regen and drain, 1 gold per login), `events.lua` (welcome / arrival messages), `npcs.lua` (patrols) |
| `maps/meadow`, `maps/cavern` | both | map 10 and map 11: one 256 m tile each, heightmap, `areas.json` with a portal each way; `.server/spawns/` (server only) places the Guard and the Bat |
| `materials/`, `models/`, `textures/` | client | primitives' materials, the animated character (`female-128-anim.glb`, from `tests/project/models`) |
| `docker-compose.override.yml` | dev | runs the `services/world` stack with this folder's Lua and maps |
| `setup_realm.cmake` | dev | copies the stack's dev CA into `certs/` and adds the realm `Starter Realm` |

The engine docs for everything used here: root `README.md` sections "Online Services" (`kast.auth`, `kast.world`,
Replication), "Lua components", "State machines", "Game GUI" (`kast.gui`), "Terrain"; and `services/world/README.md`
("Maps", "Areas", "Lua components and systems", "NPCs", "NPC spawns").

## Walkthrough

### 1. Start the stack with the sample's server content

From `services/world` (copy `.env.example` to `.env` once):

```sh
docker compose -f docker-compose.yml -f ../../samples/starter-mmo/docker-compose.override.yml up -d --build
docker compose logs world    # "maps loaded" with Meadow (10) and Cavern (11), "Lua loaded", no "Lua error"
```

The override mounts this folder as the world server's `LUA_ROOT` and `maps/` as its `MAP_ROOT`, starts new characters
on map 10 and runs no server game state machine. `services/world/docker-compose.yml` itself is unchanged. To go back
to the default content (`services/world/lua`, `tests/project/maps`): `docker compose up -d --force-recreate world`.

After editing anything under `components/`, `fsms/`, `npcs/`, `systems/` or `maps/`, restart the world server with the
same two `-f` files (`... up -d --force-recreate world`). A broken file refuses the start; `docker compose logs world`
says which.

### 2. Add the realm

```sh
cmake -P samples/starter-mmo/setup_realm.cmake
```

It copies the world server's self-signed certificate to `certs/world_dev_ca.pem` (git-ignored; the project's `caFile`)
and adds `Starter Realm` (127.0.0.1:7777) to auth's database. Run it again whenever the stack's volumes are recreated;
running it twice adds nothing.

### 3. Open it in the editor

`run.bat --project samples/starter-mmo`, or **Open** it from the Project Manager. Press Play.

### 4. Register, verify, log in

- **Create an account** opens the register screen: email + password (8+ characters), **Register**.
- In dev every mail goes to Mailpit: open http://localhost:8026, open the Kast email and copy the token from its link.
  Paste it into step 2 and press **Verify email**. Then **Back to login**, and log in.
- Errors from the service (`invalid_credentials`, `email_not_verified`, `rate_limited`, ...) show under the buttons.
- Faster while developing: **Window > Online > Test login** logs the editor in; Play then skips the login screen
  (`auth.session`).

### 5. Play

- **Realm**: pick `Starter Realm`.
- **Characters**: create one (3-16 letters), then click it.
- **World**: WASD walks. The HUD (top left) shows Health and Gold as the server replicates them: Health drains while you
  walk and regenerates while you stand. Type in the chat box and press **Send**; everyone nearby sees it. The Guard
  patrols a 10 m square just ahead of the spawn.
- **Portal**: the orange arch about 25 m east of the spawn is the `cavern_gate` portal: walk into it and the server
  moves you to the Cavern (`world.mapChanged`); the terrain and props switch and the Bat is in front of you. The arch
  west of the Cavern's arrival point takes you back.
- Losing the world connection or the auth session returns you to the login screen with the reason.

To see another player, run a second client (a built game, below), or a bot from `services/world`:

```sh
docker compose -f docker-compose.yml -f ../../samples/starter-mmo/docker-compose.override.yml run --rm bot --mode load --count 1 --behaviour idle --duration 120
```

### 6. Build and run the player

**Build > Build...** (or `kast_editor --project samples/starter-mmo --build <folder>`) writes `Starter MMO.exe`,
`game.pak` and the DLLs. Run setup step 2 first: Build checks the CA file. The built game starts at the login screen and
logs `Online Services enabled: auth http://localhost:3100, CA file 'certs/world_dev_ca.pem'`. Everything in the folder
except dot-folders ships in `game.pak`, server Lua included. Fine for a sample; a real game keeps server-only code out
of the client project.

## How it fits together

- **Screens** are scenes switched with `kast.loadScene`; the `kast.auth` session and the `kast.world` connection
  survive the switch. Buttons publish their `data-event` (`login.submit`, `realms.pick`, ...); the screen's script
  subscribes, reads the fields with `kast.gui.value` and writes status text with `kast.gui.setText`. Passwords are read
  once and the field is cleared.
- **Character select lives in the world scene**, so `world.entered` places the scene's own `NetworkLocalPlayer`. It
  hides its document and shows the HUD and chat ones.
- **Shared components**: the server's `systems/regen.lua` writes `Health` and `Wallet`; replication applies each
  `world.component` to the local player, and `scripts/hud.lua` shows the values. Both sides load the same
  `components/*.lua`, so the fields always match.
- **NPCs**: the server's spawn files place `npc` definitions whose `template` (`Guard`, `Bat`) names an entity under
  the world scene's `NPC Templates`. The client clones that entity for each NPC in view.
- **Maps**: the client's `Terrain` follows the server's map, and `scripts/world_flow.lua` shows the props tagged with
  that map's `MapProps`.
- **Why back at login**: `world_flow.lua` records the reason in the `session` game state machine, which survives
  `kast.loadScene`, and `login.lua` shows it.

## Tests

`ctest --preset debug -R starter_mmo` (registered in `cmake/starter_mmo_tests.cmake`):

- **Always run**: the editor opens the project with no warning or error; a copy plays the login screen
  (`build/debug/starter_mmo/starter_mmo_login.png`), builds, and its player starts with Online Services on.
- **Live**, against the compose stack, with a test account in the environment only:

  ```sh
  set KAST_ONLINE_EMAIL=starter-test@example.com
  set KAST_ONLINE_PASSWORD=<any test password>
  ctest --preset debug -R starter_mmo
  ```

  `starter_mmo_live_setup` restarts the stack with the override, runs `setup_realm.cmake` and registers + verifies the
  account through Mailpit if needed. Three stages then play copies of the sample with a test-only driver
  (`cmake/starter_mmo_test_driver.lua`, never part of the sample) that presses the sample's own buttons by publishing
  their events:
  - `starter_mmo_live_chars`: the character list.
  - `starter_mmo_live_world`: the Meadow with the Guard, an idle `kast_bot`, the HUD and a chat line.
  - `starter_mmo_live_cavern`: both portals, ending in the Cavern with the Bat.

  Screenshots land in `build/debug/starter_mmo/`. `starter_mmo_live_secrets` checks that no file or log holds the
  password or a token. `starter_mmo_live_teardown` restores the default world mounts, so `world_live`,
  `replication_live` and `online_live` keep working. Without docker, a running engine or the two variables, the live
  tests are skipped.
