# Starter MMO

A small but complete Kast online game, made to read and copy. You open it in the editor, then:

1. register and verify an account through the in-game GUI;
2. log in and pick a realm;
3. create a character as a Warrior, Mage, Priest or Rogue, or pick one;
4. walk the Meadow with other players and a patrolling Guard, chat, and watch the server's Health and Gold in the HUD;
5. take the portal into the Cavern, where a Bat waits, and walk back;
6. target and fight the Bat and the Guard with your class's three powers (keys 1-3 or the action bar, decided by the
   server), die and revive;
7. loot what they drop into a World of Warcraft-style 16-slot bag, and sort it by drag and drop;
8. open the character window (C), a WoW-style paper doll, and equip gear from the bag that changes your Armor, Damage
   and max Health on the server;
9. take a two-quest chain from the Elder near the Meadow spawn (kill 3 Bats, then bring 5 Bat Wings), track it in the
   quest log (L) and turn it in for gold and a Leather Cap.

The same folder is the game project (client) **and** the world server's Lua and maps: client and server share
`components/`. Only `.server/quests/` (the quest definitions) stays out of the client: dot-folders never ship.

## What is where

| Path | Side | What |
|---|---|---|
| `project.kast` | client | `Starter MMO`, starts at `scenes/login.scene`, Online Services on (`http://localhost:3100`, CA `certs/world_dev_ca.pem`), game state machine `session` |
| `scenes/` | client | `login`, `register`, `realms`, `characters` (one screen each), then one scene per zone: `meadow` (map 10, with a campfire: additive fire and alpha smoke `ParticleEmitter`s a few metres north of the spawn) and `cavern` (map 11) |
| `scripts/` | client | one Lua system per screen or concern; they talk through the event bus. `characters.lua`: the character list and creation with a class; `combat.lua`: targeting, the target marker, the combat log, a spark burst on each hit (a one-shot `ParticleEmitter` destroyed once `kast.particles.alive` is 0), tipped corpses and right clicks on them; `powers.lua`: the action bar, keys 1-3, power requests (`kast.world.send`) and their effects; `loot.lua`: the loot window and the sparkle over your corpses; `bag.lua`: the bag window, its drags and tooltips; `character.lua`: the character window (paper doll), its drags, stats and tooltips; `shrines.lua`: E uses the nearest interactable (the Meadow Shrine); `quest.lua`: the quest markers over the Elder, right clicks on it and the quest dialog; `quest_log.lua`: the quest log (L); `player.lua`: movement, stopped by the zone obstacles; `nameplates.lua`: the name and Health bar over every character and the floating damage numbers; `notice_board.lua`: the Meadow notice board's Next notice button |
| `ui/` | client | RmlUi documents, one per screen, sharing `starter.rcss` (the Kast editor palette; `box-shadow` glows on a hovered button, the picked class and ready powers); two are world-space UI ([docs/engine/world-ui.html](../../docs/engine/world-ui.html)): `nameplate.rml` (an Overlay `WorldUI` on the player, remote player and NPC prefabs) and `notice_board.rml` (the Meadow's sign, a World-mode panel you can click) |
| `components/` | **both** | `Health` (replicated to everyone in view), `Wallet` (owner only), `Class` (warrior, mage, priest or rogue; owner only, saved), `Inventory` (the bag, owner only, saved), `Equipment` (7 gear slots, owner only, saved), `Quests` (active quest ids, their objectives' progress and the completed ids, owner only, saved), `Corpse` (who may loot a dead NPC, everyone in view) |
| `items/` | **both** | one item per file (`id`, `name`, `icon`, `quality`, `maxStack`, `sellPrice`, `description`): Bat Wing, Linen Cloth, Minor Healing Potion, and the gear: Worn Shortsword, Leather Cap, Linen Tunic, Patched Trousers, Worn Boots, Cloth Gloves, Wooden Shield; `kast.items` on both sides. Which slot gear fits and its stats are `systems/inventory.lua`'s `GEAR` table (item files may hold only those seven keys) |
| `fsms/` | both | `patrol` (the Guard's machine, server: walking, waiting, combat, returning), `creature` (the Bat's machine, server: idle, combat, returning), `session` (why the client is on the login screen) |
| `npcs/` | server | the `Guard`, `Bat` and `Elder` NPC definitions; their `template` names the client entity they are shown as, `loot` what they drop. The Elder has no `Health`: friendly, it can't be targeted or hurt |
| `.server/quests/` | server only | one quest per file (`bat_trouble.lua`, `wings_for_the_elder.lua`): giver, texts, objectives, rewards, prerequisites. A dot-folder, so Build leaves it out of `game.pak`; the server reads it with `kast.definitions` |
| `systems/` | server | `classes.lua` (which classes a new character may have, `Class` and max Health on entering), `combat.lua` (the powers of each class, NPCs fighting back, roots, buffs, tagging, death and revive), `inventory.lua` (bag and equipment requests, the `GEAR` table, loot rolls, corpses), `regen.lua` (Health regen out of combat, 1 gold per login), `events.lua` (welcome / arrival messages), `npcs.lua` (patrols), `shrines.lua` (the Meadow Shrine: Health back to full and a blessing in the chat box), `quests.lua` (the quest definitions, quest requests, kill and collect progress, the turn-in, each player's markers) |
| `maps/meadow`, `maps/cavern` | both | map 10 and map 11: one 256 m tile each, heightmap, `areas.json` with a portal each way; `.server/spawns/` (server only) places the Guard, the Elder and the Bat. The Meadow also has zone objects (`objects/0_0.json`: grass and flowers, rocks, trees and the Meadow Shrine; `.server/interactables/0_0.json`, server only: the shrine's event) and its baked navtile (`.server/nav/`, the rocks, trees and shrine cut out) |
| `prefabs/`, `materials/`, `models/`, `textures/` | client | the zone objects' prefabs, built from primitives (`grass`, `flower`, `rock`, `tree`, `shrine`); the entities every zone scene shares (`sun`, `player`, `remote_player`, `npc_templates` with the Guard, the Bat and the Elder (a blue robe, white beard and a staff), `replication`, `main_camera`, `world_ui` with the HUD, chat, bag, loot, character window, quest dialog and quest log documents): edit one and both zones change; primitives' materials (the power effects: `fire`, `arcane`, `holy`, `frost`, `steel`, `shadow`), the animated character (`female-128-anim.glb`, from `tests/project/models`), item icons (`textures/icons/`, 64x64 placeholders), the character window's silhouette (`textures/character_silhouette.png`, a placeholder), particle images (`textures/particles/`: 64x64 `fire`, `smoke`, `spark`, white with the shape in alpha so the emitter tints them; generated once by a throwaway script) |
| `docker-compose.override.yml` | dev | runs the `services/world` stack with this folder's Lua and maps |
| `setup_realm.cmake` | dev | copies the stack's dev CA into `certs/` and adds the realm `Starter Realm` |

The engine docs for everything used here: [Online Services](../../docs/engine/online-services.html) (`kast.auth`,
`kast.world`, Replication), [Lua components](../../docs/engine/lua-components.html),
[State machines](../../docs/engine/state-machines.html), [Game GUI](../../docs/engine/game-gui.html) (`kast.gui`),
[Terrain](../../docs/engine/terrain.html); and the World Server's [Maps](../../docs/services/world/maps.html),
[Areas](../../docs/services/world/areas.html), [Lua components and systems](../../docs/services/world/lua.html),
[NPCs](../../docs/services/world/npcs.html) and [NPC spawns](../../docs/services/world/npcs.html#npc-spawns).

## Walkthrough

### 1. Start the stack with the sample's server content

From `services/world` (copy `.env.example` to `.env` once):

```sh
docker compose -f docker-compose.yml -f ../../samples/starter-mmo/docker-compose.override.yml up -d --build
docker compose logs world    # "maps loaded" with Meadow (10) and Cavern (11), "Lua loaded", no "Lua error"
```

`--build` matters after a World Server update (this sample needs one that loads `items/`, NPC `loot` and has
`kast.definitions`, which `systems/quests.lua` reads its quests with; the log then says `quests loaded` with `count` 2): an older image
refuses the sample's NPC definitions and the `world` container never gets healthy.

The override mounts this folder as the world server's `LUA_ROOT` and `maps/` as its `MAP_ROOT`, starts new characters
on map 10 and runs no server game state machine. `services/world/docker-compose.yml` itself is unchanged. To go back
to the default content (`services/world/lua`, `tests/project/maps`): `docker compose up -d --force-recreate world`.

After editing anything under `components/`, `fsms/`, `items/`, `npcs/`, `systems/`, `.server/quests/` or `maps/`, restart the world server with the
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
- **Characters**: pick a class (the button turns `#ffcc00` and a line describes it), type a name (3-16 letters),
  **Create character**, then click it: `Play <Name> (<Class>)`. Create without a class only says `Pick a class first.`;
  the class can't be changed later. A character made before classes existed plays as a Warrior.
- **World**: World of Warcraft-style controls. W runs forward, S backpedals, A/D turn, Space jumps. Hold the left
  mouse button and drag to look around the character; hold the right button to turn the camera and the character (A/D
  then strafe). The wheel zooms. A press on the chat box or HUD doesn't move the camera. The HUD (top left) shows your
  name and class, Health and Gold as the server replicates them. Press **Enter** (or click the chat box), type and press **Enter** again (or **Send**); plain text is
  said to everyone nearby. Slash commands (`scripts/chat.lua`): `/s` say, `/y` yell (the whole map within 300 m),
  `/me waves` (an emote: "Name waves"), `/w name text` whisper (any map), `/1 /2 /3 text` the General / Trade / LFG
  channels, `/join Trade`, `/leave Trade`, `/z text` everyone on this map, `/ignore name`, `/unignore name`, and for GM
  accounts `/mute name minutes` and `/unmute name`. Lines are coloured by kind (`[General] Name: ...`, `Name whispers:
  ...`, `To Name: ...`, `Name yells: ...`); whispers, channels and zone chat go through the stack's chat service
  (`services/chat`), and if it is down the box says so while say keeps working. The Guard
  patrols a 10 m square just ahead of the spawn.
- **Nameplates** (`scripts/nameplates.lua`, world-space UI): every character shows its name over its head, and a Health
  bar once the server has sent its Health: you, other players, the Guard and the Bat. The Elder, being friendly, shows
  its name only; a dead character's nameplate hides until it revives. When a character's Health drops, the loss floats up
  over it in red (`kast.gui.worldText`) and fades within a second; Health going up (regeneration, Heal) shows nothing.
  Your own nameplate is on too; to hide it, remove the `WorldUI` component from `prefabs/player.prefab`.
- **Notice board**: a few metres from the spawn stands a board (a World-mode panel in the scene, `ui/notice_board.rml`); hover
  its **Next notice** button (it lights up) and click it to read the next of three notices. Clicking the board neither
  targets nor turns the camera.
- **Combat** (tab-target): **Tab** cycles the NPCs in view, nearest first, within 40 m; a **left click** (without
  dragging) on an NPC selects it; **Esc** clears the target. The target gets a gold disc under it and a frame at the top
  centre with its name, Health and a round portrait (a placeholder per kind, NPC or player). Keys typed into the chat
  box do nothing.
- **Classes and powers**: each class has three powers on keys **1**, **2** and **3**, also shown on the action bar at
  the bottom (in a narrow window, right of the chat box); clicking a button is the same as its key. All are instant,
  with cooldowns only (no mana, rage or energy). A power used starts its own cooldown and a 1 s global cooldown on all
  three: the buttons grey out and count down the seconds, under a dark wipe that sweeps clockwise as the cooldown runs out. Damage powers need a living NPC target in range (melee 3 m,
  ranged 25 m); heals and buffs are on yourself.

  | Class | Health | 1 | 2 | 3 |
  |---|---|---|---|---|
  | Warrior | 140 | Strike: melee, 12 (1.5 s) | Heroic Slam: melee, 25 (8 s) | Shield Wall: NPC damage halved for 6 s (30 s) |
  | Mage | 80 | Firebolt: ranged, 14 (2 s) | Frost Nova: NPCs within 8 m can't move for 4 s, no damage (20 s) | Arcane Blast: ranged, 30 (10 s) |
  | Priest | 90 | Smite: ranged, 10 (1.5 s) | Heal: +40 Health, up to your max (10 s) | Power Shield: absorbs the next 30 NPC damage, gone after 10 s (25 s) |
  | Rogue | 110 | Sinister Strike: melee, 11 (1.2 s) | Backstab: melee, 30, only from behind the target (8 s) | Evasion: 50% chance to dodge NPC hits for 8 s (30 s) |

  Ranged powers send a small glowing ball from you to the target; Frost Nova flashes a frost ring, Heal a glow, and
  Shield Wall, Power Shield and Evasion keep a see-through sphere around you while they last. Everyone nearby sees them.
  A rooted NPC still hits you if you are within its reach. Reviving restores your full Health (your class's plus your
  gear's Stamina). Gear adds to these numbers (below): the Worn Shortsword makes Strike 15.
  - Take the portal (below) to the Cavern: the Bat stands 8 m ahead. Target it and press 1 as a Warrior: `* Out of
    range` in the chat box. Walk up to it (within 3 m) and press 1 again: `You cast Strike on Bat for 12`, `Bat dies`.
    Its corpse tips over and stays until it is looted empty or for 60 s; the Bat comes back at its spot 30 s after the
    corpse goes. A Mage can do the same from 20 m with Firebolt; a Rogue's Backstab from in front of the Bat says
    `* Not behind target`.
  - The Guard fights back: hit it and it chases you at 5 m/s and hits you for 5 every 2 s (`Guard hits you for 5`; 2
    under Shield Wall; `Power Shield absorbs 5`; `You dodge Guard's attack`). Run more than 30 m from its spawn point and
    it gives up, walks home, heals to full and patrols again.
  - Health regenerates (2 a second) only after 5 s without dealing or taking a hit. At 0 Health you die: you can't move
    or use powers and NPCs leave you; 5 s later you revive at the map's spawn point with full Health. A character that
    logs out dead dies again on login and revives the same way.
  - Every power is a request (`kast.world.send("combat.power", {power = "firebolt", target = id})`); the server's
    `systems/combat.lua` checks your class, the cooldowns, the target, range and that both sides are alive, and decides
    what happens. The combat log in the chat box shows what it sends back; a refusal is a `* ...` line (`* Not ready
    yet`, `* Not ready yet (global cooldown)`, `* Out of range`, `* Invalid target`, `* Your target is dead`, `* Not
    behind target`, `* You are dead`).
- **Loot**: an NPC drops loot once, when it dies, for the player who hit it first (its tagger). Bat: Bat Wing 80%,
  Linen Cloth 40%, Worn Boots 10%, Cloth Gloves 10%, 1-3 gold. Guard: Linen Cloth 60% (1-2), Minor Healing Potion 25%,
  Worn Shortsword 10%, Leather Cap 10%, Linen Tunic 10%, Patched Trousers 10%, Wooden Shield 5%, 5-15 gold.
  - Only the tagger sees a gold sparkle bobbing over the corpse; anyone else gets `* You can't loot that`.
  - **Right click** the corpse (press and release without dragging) within 5 m: the loot window opens with its gold and
    items (icon, name in quality colour, count). Click an entry to take it, or **Take All**. Further away:
    `* Too far away`. Gold goes to the HUD's Gold; items into the bag, onto stacks of the same item first.
  - With no room, the item stays in the corpse and the chat box says `* Inventory is full`.
  - The window closes when the corpse is empty (it then despawns), when it despawns after 60 s, when you walk more
    than 5 m away, or on **Esc**.
- **Bag**: **B** opens and closes it (typed into the chat box, B is just a "b"). 16 slots in a 4x4 grid: icon, border
  in the item's quality colour (grey poor, white common, green uncommon), count when over 1. Hover a slot for its
  tooltip: name, stack, description, sell price (no vendors yet). Drag a stack (the camera stays put):
  - onto an empty slot: moves it; with **Shift** held: moves half of it (a split);
  - onto a different item: swaps them; onto the same item: merges up to its stack size (20 Bat Wings, 5 potions, 1
    sword), the rest stays;
  - outside the bag (the world, another window): `Destroy <item>?` with **Destroy** / **Cancel**.

  The bag is saved with the character. Every change is a request (`kast.world.send("inventory.move", ...)`) that the
  server's `systems/inventory.lua` checks (slots, the item it expects, stack sizes, loot owner, range, alive); a refused
  one says why in the chat box and changes nothing. Gear's tooltip also shows its slot and stats.
- **Character window**: **C** opens and closes it (typed into the chat box, C is just a "c"). A World of Warcraft-style
  paper doll: your name and class on top, a silhouette with seven slots around it (Head, Chest, Legs on the left,
  Hands and Feet on the right, Main Hand and Off Hand below; an empty slot shows its name faintly, a filled one the
  item's icon in its quality border), and your Max Health, Armor and Damage bonus under it. Hover a slot for its
  tooltip: name, slot, stats, description. Drag (the camera stays put):
  - a bag item onto its slot: equips it; onto an occupied slot: swaps, the old item goes where the new one was;
  - an equipped item onto an empty bag slot: unequips it; onto gear for the same slot: swaps them; onto any other item:
    `* That bag slot is taken`; onto the bag's frame: the first empty slot. No room: `* Inventory is full`;
  - onto the wrong slot: `* Can't equip that there`. Dead: `* You are dead`. Within 5 s of dealing or taking a hit
    (the same window Health regeneration waits for): `* Can't do that in combat`. A refusal changes nothing.

  | Item | Slot | Stats | Quality |
  |---|---|---|---|
  | Leather Cap | Head | 1 Armor, 1 Stamina | common |
  | Linen Tunic | Chest | 2 Armor, 2 Stamina | common |
  | Patched Trousers | Legs | 1 Armor, 1 Stamina | common |
  | Worn Boots | Feet | 1 Armor, 1 Stamina | common |
  | Cloth Gloves | Hands | 1 Armor, 1 Stamina | common |
  | Worn Shortsword | Main Hand | 3 Damage | uncommon |
  | Wooden Shield | Off Hand | 2 Armor, 1 Stamina | uncommon |

  Stats are flat and the server applies them: **Armor** takes 1 per point off every NPC hit, never below 1, before
  Shield Wall halves it and Power Shield absorbs it (Evasion's dodges are unchanged); the full set's 8 Armor turns the
  Guard's 5 into 1. **Damage** adds to every damage power, melee and ranged (Strike 12 -> 15, Firebolt 14 -> 17); heals
  and buffs are unchanged. **Stamina** gives 5 max Health a point on top of the class's (a Warrior in the full set: 175).
  Equipping raises the max but not your current Health; unequipping lowers the max and clamps your Health to it.
  Gear is saved with the character. Any class can wear anything; Main Hand and Off Hand are independent.
- **Quests**: the **Elder**, a robed figure with a white beard and a staff, stands about 6 m north-west of the spawn. It
  is friendly: Tab and left clicks skip it and powers on it say `* Invalid target`. Over its head floats a yellow **!**
  when it has a quest for you, a yellow **?** when one is ready to turn in, a grey **?** while one is in progress, nothing
  otherwise; each player sees their own.
  - **Right click** the Elder (press and release without dragging) within 5 m: the quest dialog opens (further away:
    `* Too far away`). It lists the Elder's quests for you; the chosen one shows its text, objectives, rewards (gold,
    item icons with quality-coloured names) and **Accept** or **Complete**. **Close**, **Esc** or walking more than 5 m
    away closes it, and it closes by itself when nothing is left to show.
  - **Bat Trouble**: kill 3 Bats in the Cavern, 10 gold. Every player who hit a Bat and has the quest gets the kill,
    not only the one who tagged it. Then **Wings for the Elder** (only after Bat Trouble): bring 5 Bat Wings, 15 gold
    and a Leather Cap. Its progress is the Bat Wings in your bag right now: wings you already had count, and destroying
    them lowers it. Turning it in takes the 5 wings; with no room left for the cap it says `* Inventory is full` and
    nothing changes. Quests can't be repeated.
  - **L** opens and closes the quest log (typed into the chat box, L is just an "l"): your active quests (up to 20) with
    their progress (`Bats slain: 2/3`, `Bat Wing: 4/5`); click one for its text and rewards, **Abandon** drops it and its
    progress (take it again from the Elder).
  - Quests, their progress and the completed ones are saved with the character. Every action is a request
    (`kast.world.send("quest.accept", {npc, quest})`, ...) that `systems/quests.lua` checks; a refusal is a `* ...` line
    (`* You can't take that quest yet`, `* You are already on that quest`, `* You have already completed that quest`,
    `* Quest not complete`, `* Quest log is full`, `* You are dead`, ...). Objective updates add no chat lines: the log
    and the dialog show them.
- **Portal**: the orange arch about 25 m east of the spawn is the `cavern_gate` portal: walk into it and the server
  moves you to the Cavern (`world.mapChanged`); the client loads `scenes/cavern.scene` and the Bat is in front of you.
  The arch west of the Cavern's arrival point takes you back to `scenes/meadow.scene`.
- **Meadow objects** (zone objects, [Engine, Terrain](../../docs/engine/terrain.html#objects)): grass and flowers you
  walk through; rocks, trees and the Meadow Shrine (north-west of the spawn) that stop you, and you slide along them.
  The world server knows only their shapes, cut out of the Meadow's navmesh, so it would correct a move into one too.
  Walk within 3 m of the shrine: the chat box says `[E] Use the Meadow Shrine`. Press **E**: `scripts/shrines.lua`
  calls `kast.interact`, the server checks you are on the Meadow and in range, and `systems/shrines.lua` (subscribed to
  the shrine's `shrine.activated` event) restores your Health and answers `* The shrine hums softly. You feel
  refreshed.` Elsewhere, E says `There is nothing here to use.` Edit them in the editor's
  [Zone tab](../../docs/editor/zone-builder.html#objects) (open `scenes/meadow.scene`: its Terrain entity names the Meadow, map 10),
  then re-bake (its **Bake** section) and restart the world server.
- Losing the world connection or the auth session returns you to the login screen with the reason.

To see another player, run a second client (a built game, below), or a bot from `services/world` (`--char-option`
gives its character a class; without one this sample refuses to create it):

```sh
docker compose -f docker-compose.yml -f ../../samples/starter-mmo/docker-compose.override.yml run --rm bot --mode load --count 1 --behaviour idle --duration 120 --char-option class=warrior
```

### 6. Build and run the player

**Build > Build...** (or `kast_editor --project samples/starter-mmo --build <folder>`) writes `Starter MMO.exe`,
`game.pak` and the DLLs. Run setup step 2 first: Build checks the CA file. The built game starts at the login screen and
logs `Online Services enabled: auth http://localhost:3100, CA file 'certs/world_dev_ca.pem'`. Everything in the folder
except dot-folders ships in `game.pak`, server Lua included, but not the quests (`.server/quests/`): players can't read
what a quest asks or gives before the server tells them. Fine for a sample; a real game keeps all server-only code out
of the client project, in dot-folders or elsewhere.

## How it fits together

- **Screens** are scenes switched with `kast.loadScene`; the `kast.auth` session and the `kast.world` connection
  survive the switch. Buttons publish their `data-event` (`login.submit`, `realms.pick`, ...); the screen's script
  subscribes, reads the fields with `kast.gui.value` and writes status text with `kast.gui.setText`. Passwords are read
  once and the field is cleared.
- **One scene per zone**: `scenes/characters.scene` enters the world; `scripts/world_flow.lua` (in it and in every
  zone scene) then loads the scene of the player's map from its table (`10` -> `meadow.scene`, `11` ->
  `cavern.scene`), again on each `world.mapChanged` to another map. A zone's scene holds its `Terrain` (the map it
  names), its own props (the portal arch, rocks, stalagmites) and instances of the shared prefabs. Replication carries
  the player's position, the replicated components and the players and NPCs in view into the new scene and tells its
  scripts `world.entered` and `world.component` again; the one-time `welcome`, `class.powers` and `equipment.gear`
  events are caught up with `kast.subscribe(..., {replay = true})` ([Online
  Services](../../docs/engine/online-services.html#replication)). A new map needs its scene and a line in that table.
  The chat box starts empty in each zone.
- **Shared components**: the server's `systems/classes.lua` and `regen.lua` write `Class`, `Health` and `Wallet`;
  replication applies each
  `world.component` to the local player, and `scripts/hud.lua` shows the values. Both sides load the same
  `components/*.lua`, so the fields always match.
- **Classes**: the engine's `kast.world.createCharacter(name, options)` carries a table of strings it knows nothing
  about; this sample sends `{class = "mage"}`. The world server checks its size, then asks the server Lua's
  `validateCharacter` (here `systems/classes.lua`: exactly `class`, one of four; anything else is `options_invalid`),
  saves it with the character and returns it in the character list (`options`). On entering, `classes.lua` reads
  `kast.get(e, "Character").options.class` into the saved `Class` component, and `combat.lua` sends the owner its
  powers (`class.powers`), which `scripts/powers.lua` puts on the action bar. The powers are defined once, in
  `systems/combat.lua`: client scripts can't load server files (no `require` in the sandbox), so the client learns
  names, keys and cooldowns from that event instead.
- **NPCs**: the server's spawn files place `npc` definitions whose `template` (`Guard`, `Bat`) names an entity under
  `NPC Templates` (`prefabs/npc_templates.prefab`, in each zone scene). The client clones that entity for each NPC
  in view.
- **Items and loot**: both sides load `items/` (`kast.items`). The server rolls `kast.npcDef(name).loot` on `npc.died`
  (published by `systems/combat.lua`) and keeps the contents to itself: clients only see `Corpse {owner}`, and the owner
  asks for the contents (`loot.open`). The bag is two parallel arrays (`items`, `counts`) because component fields
  can't hold tables.
- **Equipment**: `Equipment` is one array of 7 item ids, in `components/equipment.lua`'s slot order. Item files may hold
  only the engine's seven keys, so which slot an item fits and its stats are the server's `GEAR` table in
  `systems/inventory.lua`, sent to the owner on entering (`equipment.gear`, like `class.powers`) for the window and the
  tooltips. Equip and unequip live in `inventory.lua` because they move items between the bag and the equipment in one
  step, and system files can't share `readBag` / `writeBag`. After every change (and on entering) it publishes
  `equipment.stats {entity, armor, damage, stamina}` on the server bus: `combat.lua` keeps each player's Armor and
  Damage from it, `classes.lua` sets `Health.max` (class base + 5 per Stamina). Neither reads the gear itself.
- **Quests**: the definitions are server-only (`.server/quests/`, read once by `systems/quests.lua` with the World
  Server's `kast.definitions`), so the client learns a quest only from the server's answers: `quest.dialog` (the giver's
  quests for you, with text, objectives and rewards) and `quest.log` (your active ones). Your state is the `Quests`
  component (three primitive arrays: active ids, 4 progress counts per active quest, completed ids), owner-only, which
  the log and dialog read for progress. Kill credit comes from `systems/combat.lua`'s `npc.died {npc, definition,
  tagger, damagers}` on the server bus; collect progress from `systems/inventory.lua`'s `inventory.changed {entity}`;
  the turn-in asks `inventory.lua` for one all-or-nothing `inventory.exchange` (take the wings, give the cap and the
  gold), since only it changes the bag. The markers are worked out per player on the server and sent to that player
  alone (`quest.markers`); `scripts/quest.lua` builds the ! and ? from cubes over every clone of that giver.
- **Drag and drop** is the engine's: a slot has RCSS `drag: clone` and `data-drag="bag.slot"`, so RmlUi drags publish
  `bag.slot.start` / `bag.slot.drop` with the target slot and Shift ([Game GUI](../../docs/engine/game-gui.html#drag-and-drop)).
  Drops carry the target's document, so a bag slot dropped on `ui/character.rml` is `scripts/character.lua`'s to equip
  (the bag ignores it), and equipment slots (`data-drag="equip.slot"`) dropped on `ui/bag.rml` unequip.
- **Why back at login**: `world_flow.lua` records the reason in the `session` game state machine, which survives
  `kast.loadScene`, and `login.lua` shows it.

## Tests

`ctest --preset debug -R starter_mmo` (registered in `cmake/starter_mmo_tests.cmake`):

- **Always run**: the editor opens the project with no warning or error; a copy plays the login screen
  (`build/debug/starter_mmo/starter_mmo_login.png`); a hover glows Log in (`starter_mmo_login_hover.png`) and the GUI
  Builder shows the HUD's ready powers glowing (`starter_mmo_hud_glow.png`), both pixel-probed; it builds, and its player starts with Online Services on.
  `starter_mmo_hit_sparks` plays the Meadow offline with a test-only driver (`cmake/starter_mmo_hit_driver.lua`) that
  publishes the server's `combat.hit`: 24 sparks, their entity destroyed once none is alive, then a second burst
  (`starter_mmo_hit_sparks.png`). `starter_mmo_pak_server_only` checks the built `game.pak` holds nothing from `.server/` (no quest
  definitions or quest text) but does hold `scripts/quest.lua`. `starter_mmo_lists` plays it offline with
  `cmake/starter_mmo_lists_driver.lua` (test only): 15 chat lines and 25 quests in the bound (`data-for`) chat and quest
  log, no row cap (`starter_mmo_lists.png`). `starter_mmo_hud_masks` plays it offline with
  `cmake/starter_mmo_hud_masks_driver.lua` (test only: the Guard targeted, a full chat, Firebolt cooling down) and
  pixel-probes the round target portrait, the chat's top fade and the cooldown wipes (`starter_mmo_hud_masks.png`).
  `starter_mmo_nameplates` plays the Meadow offline with `cmake/starter_mmo_nameplates_driver.lua` (test only) in the
  world client's place: a Guard, the Elder, a Bat and another player spawn, the Guard is hit for 12 and the Bat dies; the
  shot (`starter_mmo_nameplates.png`) is pixel-probed for each bar (the Elder's missing), and the log for the Bat's
  hidden plate and the two damage numbers. `starter_mmo_notice_board_hover` / `_click` hover and click the board's
  button with real mouse events (highlighted; the next page). `starter_mmo_nameplates_perf` builds a copy that starts in
  the Meadow with 100 Guards in view, each with its nameplate, runs the player without vsync and fails if the average
  frame time is over 16.7 ms (60 fps; optimized builds only; the number is printed).
- **Live**, against the compose stack, with a test account in the environment only:

  ```sh
  set KAST_ONLINE_EMAIL=starter-test@example.com
  set KAST_ONLINE_PASSWORD=<any test password>
  ctest --preset debug -R starter_mmo
  ```

  `starter_mmo_live_setup` restarts the stack with the override, runs `setup_realm.cmake` and registers + verifies the
  account through Mailpit if needed. The stages then play copies of the sample with a test-only driver
  (`cmake/starter_mmo_test_driver.lua`, never part of the sample) that presses the sample's own buttons by publishing
  their events:
  - `starter_mmo_live_chars`: the character list.
  - `starter_mmo_live_world`: the Meadow with the Guard, an idle `kast_bot`, the HUD and a chat line.
  - `starter_mmo_live_nameplates`: the idle bot's and the Guard's nameplates, then Strikes on the Guard: its bar
    shrinks and damage numbers show and go.
  - `starter_mmo_live_cavern`: both portals, ending in the Cavern with the Bat.
  - `starter_mmo_live_travel`: enters in `meadow.scene`, walks the portal into `cavern.scene` (Terrain map 11, the
    Bat cloned, HUD and chat shown, the player where the server put it) and back into `meadow.scene` (the Guard).
  - `starter_mmo_live_camera`, `_camera_zoom`, `_camera_orbit`: `scripts/player.lua`'s camera behind the character,
    zoomed out and orbited by a left drag (the editor's `--sim-mouse`).
  - `starter_mmo_live_rock`: real W presses (`--sim-key`) walk the character into a Meadow rock; it stops 1.65 m from
    the rock's centre (its 1.2 m radius and the character's 0.45 m), or up to a frame's step short of that.
  - `starter_mmo_live_shrine`: at the Meadow Shrine, `[E] Use the Meadow Shrine`, a real E press, and the server's
    blessing in the chat box.
  - `starter_mmo_live_combat_target`: a left click selects the Bat (target frame and marker).
  - `starter_mmo_live_combat`: Tab, Esc, Tab, then Strike (the action bar's first button) until the Bat dies.
  - `starter_mmo_live_combat_guard`: three Strikes on the Guard, which hits back.
  - `starter_mmo_live_death`, `starter_mmo_live_revive`: the Guard kills the Warrior `Fallen`; dead, then revived at
    the spawn point with full Health (140).
  - Loot and bag stages use a test-only server system, `cmake/starter_mmo_test_server.lua`, which the live setup adds
    to a copy of this folder's server Lua (a third compose file in `build/debug/starter_mmo/live/`): `test.seed` makes
    the Bat's roll known (1 gold, Bat Wing, Linen Cloth) and `test.bag` sets a bag.
  - `starter_mmo_live_loot_window`: a real right click (`--sim-mouse` with `--sim-on-log`) on `Looter`'s Bat
    corpse, one sparkle, the loot window.
  - `starter_mmo_live_loot_rejects`: `Rival` sees that corpse without a sparkle and is refused (`not_owner`); bad slots
    are refused; its own corpse is refused from 8.5 m (`range`), then taken from 2.5 m.
  - `starter_mmo_live_loot`: Take All: `gold +1`, Bat Wing and Linen Cloth in the bag, the corpse gone, the bag open.
  - `starter_mmo_live_bag_persist`: `Looter` again: the same bag after the relog, slot 1's tooltip.
  - `starter_mmo_live_bag_full`: a bag of 16 full stacks: the gold is taken, the items stay, `* Inventory is full`.
  - `starter_mmo_live_bag_move`, `_swap`, `_merge`, `_split`, `_destroy_confirm`, `_destroy`: `Sorter`'s drags, one
    step further each, replayed as the bag's `bag.slot.drop` events.
  - `starter_mmo_live_bag_drag_mouse`: a real mouse drag (`--sim-mouse` from slot 1 to slot 3) moves the stack.
  - `starter_mmo_live_bag_keys`: real key presses (`--sim-key`): B opens the bag; with the chat field focused, B types
    "b" and the bag stays open.
  - `starter_mmo_live_class_pick`: Create with no class sends nothing; Mage picked, highlighted, its description.
  - `starter_mmo_live_class_warrior`: a new Warrior against the Guard: `* Out of range`, Shield Wall, Strike refused by
    the global cooldown, Strike 12, Strike refused by its cooldown, Heroic Slam 25, the Guard's hits halved to 2; the
    screenshot shows the shield and the greyed buttons.
  - `starter_mmo_live_class_mage_bolt`, `_class_mage`: a Firebolt in flight; Firebolt 14, Arcane Blast 30 and Frost
    Nova rooting the oncoming Guard.
  - `starter_mmo_live_class_priest`: Power Shield absorbing the Guard's hits, Smite 10, Heal.
  - `starter_mmo_live_class_rogue`, `_class_backstab`: Evasion dodging the Guard (seeded), Sinister Strike 11; Backstab
    refused in front of the Cavern's Bat, 30 from behind it.
  - `starter_mmo_live_class_list`: the character list with each character's class.
  - `starter_mmo_live_legacy` (`cmake/starter_mmo_live_legacy.cmake`): `Veteran` is stripped of its class in MySQL (as if
    made before classes), enters as a Warrior with 140 max Health, and MySQL then holds its saved Warrior `Class`.
  - Equipment stages start bare (the test server's `test.equipment`) with a known bag (`test.bag`):
    `starter_mmo_live_equip_open`: real C presses (`--sim-key`) open the character window; with the chat field focused C
    types "c" and the window stays open. `starter_mmo_live_equip_drag`: a real mouse drag (`--sim-mouse`) of the Worn
    Shortsword from bag slot 1 onto Main Hand; the stats show Damage +3 and the camera didn't turn.
    `starter_mmo_live_equip_tooltip`: hovering the Leather Cap in the bag (slot and stats in its tooltip).
    `starter_mmo_live_equip_rules`: the wrong slot (refused by the client, then by the server), a full bag and, after a
    Strike on the Guard, in combat, each in the chat box. `starter_mmo_live_equip_combat`: `Brawler` in sword, tunic
    and shield: 155 max Health, Strike 15, the Guard's hits down to 1. `starter_mmo_live_equip_persist`: `Brawler`
    again after the relog: the same gear, 155 max Health, Armor 4, Damage +3, both windows open.
  - Quest stages (the test server's `test.quests` sets a `Quests` state, `test.health` a Health, `test.bag` the 5 Bat
    Wings; `Quester` goes one step further each): `starter_mmo_live_quest_dialog`: the Elder's yellow ! (its two cubes),
    `* Too far away` from 6.4 m, then a real right click (`--sim-mouse`) opens the dialog. `starter_mmo_live_quest_log`:
    Accept, 0/3 and the grey ?; real keys: L opens the log, L typed into the focused chat field is an "l".
    `starter_mmo_live_quest_kills`: three Bats killed and looted in the Cavern (1/3, 2/3, 3/3), the yellow ? back in the
    Meadow, the turn-in (+10 gold), Wings for the Elder taken at 3/5 (the looted wings count), Bat Trouble refused as
    completed. `starter_mmo_live_quest`: 5 Bat Wings, the log at 5/5, the turn-in: wings gone, +15 gold, the Leather Cap
    in the bag, nothing left to offer. `starter_mmo_live_quest_rejects` (`Rejecter`): unknown quest, prerequisites
    unmet, turn-in and abandon of a quest not taken, already active, incomplete turn-in, a 21st quest, dead, out of
    range and already completed, each refused with its reason; Abandon in the log resets 2/3 and the quest is taken
    again at 0/3; a full bag refuses the turn-in with `* Inventory is full` and changes nothing.
    `starter_mmo_live_quest_persist`: `Rejecter` again after the relog: Wings for the Elder at 5/5, Bat Trouble
    completed, the yellow ?.
  The test-time compose file also lets the test account hold 24 characters, since each stage has its own.

  The engine's drag and drop itself is checked by `ctest -R ui_drag` (a real `--sim-mouse` drag, with and without
  Shift, in `tests/project/ui_drag.scene`); the server's bag and loot rules by `world_self_check` (`starter-mmo loot`), and every class and power rule (damage,
  ranges, cooldowns, the global cooldown, roots, shields, dodges, Backstab, refusals) by its `starter-mmo classes`
  checks. Its `starter-mmo equipment` checks cover equip, unequip and both swaps, every refusal (wrong slot, occupied,
  full, dead, in combat, bad indices, a mismatched item), saved gear cleanup, Armor (with its minimum and its order
  before Shield Wall and Power Shield), Damage on melee and ranged powers, Stamina's max Health with clamping, revive
  and login with gear, and the gear saved across a relog. Its `starter-mmo quests` checks (several players beside the Elder
  and Bats made with more Health) cover kill credit for every player who hit a Bat (two players credited for one Bat,
  one who never hit it not), every quest refusal, markers and `Quests` reaching their owner only, collect progress,
  both turn-ins, the full-bag refusal and the quests saved across a relog.

  The combat stages take their screenshot a moment after the driver logs its shot line (the editor's
  `--screenshot-on-log`).

  Screenshots land in `build/debug/starter_mmo/`. `starter_mmo_live_secrets` checks that no file or log holds the
  password or a token. `starter_mmo_live_teardown` restores the default world mounts, so `world_live`,
  `replication_live` and `online_live` keep working. Without docker, a running engine or the two variables, the live
  tests are skipped.
