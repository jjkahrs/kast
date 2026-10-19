-- TEST ONLY (ctest starter_mmo_live_*, cmake/starter_mmo_live_play.cmake): added to a copy of samples/starter-mmo's
-- realms and world scenes, never to the sample itself. It plays the sample's screens the way a player would, by
-- publishing the events their buttons publish (realms.pick, characters.pick / .create, chat.send) and filling text
-- fields with kast.gui.setText, so the sample's own handlers do the work, then logs what arrives for the test to check.
-- Stages: "chars" stops at the character list; "world" enters as CHARACTER, steps beside the idle bot at the spawn and
-- chats; "cavern" enters as CHARACTER and walks through the portals until it has crossed both ways and is in the Cavern.
-- "camera" enters as CHARACTER and stands still, for screenshots of scripts/player.lua's camera (with --sim-mouse).
-- Combat stages (scripts/combat.lua, the server's systems/combat.lua) face north on entering (the camera looks north)
-- and get to their map through the portals first. "combat_target" stands 2.5 m north of the Cavern's Bat, so the
-- Bat is on the view's centre line for --sim-mouse's click. "combat" stands there too, targets with the Tab key's event,
-- clears and targets again, and attacks until the Bat dies. "combat_guard" walks up to the Meadow's Guard from the south
-- and hits it ATTACKS_ON_GUARD times; "death" and "revive" hit it once and stand until it kills them.
-- Loot and bag stages (scripts/loot.lua, scripts/bag.lua, the server's systems/inventory.lua) use the test-only server
-- system cmake/starter_mmo_test_server.lua: "test.bag" sets the bag, "test.seed" makes the Bat's loot roll known (SEED:
-- 1 gold, a Bat Wing, Linen Cloth). They stand where "combat" does, wait for a living Bat, kill it and then:
-- "loot_window" logs CLICK (--sim-on-log right-clicks the corpse) and shows the loot window; "loot_rejects" first
-- tries the corpse loot_window left (not its owner: no sparkle, refused), sends bad bag requests, then kills the Bat
-- itself and asks for its loot from 8.5 m (refused) before taking it all; "loot" takes it all and opens the bag;
-- "bag_full" fills the bag first, so the loot stays in the corpse. "bag_persist" (Meadow) logs the bag after a relog,
-- opens it and logs HOVER (--sim-on-log hovers slot 1 for its tooltip). The bag stages (BAG_STEPS, Meadow) set a
-- known bag and replay the bag's drag events (bag.slot.drop, as RmlUi publishes them) up to their own step.
-- "bag_drag_mouse" (Meadow) sets the same known bag and logs DRAG: --sim-mouse drags slot 1 onto slot 3 for real.
-- "bag_keys" (Meadow) logs KEYS: --sim-key presses B (the bag opens), the driver then focuses the chat field
-- (kast.gui.focus) and --sim-key presses B again: it types "b" there and the bag stays open.
-- Classes and powers (scripts/characters.lua, scripts/powers.lua, the server's systems/classes.lua and combat.lua): a
-- new character gets STAGE_CLASS's class (default warrior) by the class button's event before Create. "class_pick"
-- presses Create with no class (nothing may be sent), then picks Mage, for the screenshot. The class stages run
-- CLASS_STEPS: walk to a spot near the Meadow's Guard (or in front of / behind the Cavern's Bat), Tab, and press power
-- keys by the action bar's event ("combat.power" {id = "power_<key>"}), shooting at the step marked shot. "legacy"
-- stands still and logs its Class (cmake/starter_mmo_live_legacy.cmake strips the class first).
-- Zone objects (maps/meadow/objects, scripts/player.lua, scripts/shrines.lua): "rock" walks to ROCK_SPOT, faces north
-- and logs KEYS: --sim-key presses W over and over, scripts/player.lua walks the character north into the rock, which
-- stops it; then the driver logs where it stopped. "shrine" walks to SHRINE_SPOT, faces the Meadow Shrine and logs
-- PRESS: --sim-key presses E, scripts/shrines.lua activates it and the server's answer shows in the chat box.
-- Equipment (scripts/character.lua, the server's systems/inventory.lua, combat.lua, classes.lua; Meadow): each stage
-- starts bare (the test server's "test.equipment") with a known bag. "equip_open" logs KEYS: --sim-key presses C (the
-- character window opens), the driver focuses the chat field and C again types "c" there, the window stays open.
-- "equip_drag" opens the bag and the character window and logs DRAG: --sim-mouse drags the Worn Shortsword from bag
-- slot 1 onto Main Hand for real. "equip_rules" and "equip_combat" run CLASS_STEPS with act steps (a drop replayed as
-- RmlUi publishes it, a raw request, a known bag): the refusals, then the Guard. "equip_persist" (equip_combat's
-- character) logs its gear after the relog and opens both windows. "equip_tooltip" (equip_drag's character, the Leather
-- Cap left in bag slot 2) opens the bag and logs HOVER: --sim-mouse hovers that slot for its tooltip.
-- "SHOT" log lines are for --screenshot-on-log: the moment a stage's screenshot should show.
local STAGE, CHARACTER = "@STAGE@", "@CHARACTER@"
local REALM = "Starter Realm"
local SPEED = 5 -- m/s, under the world server's 10 m/s limit
local MEADOW, CAVERN = 10, 11
local GATES = {[MEADOW] = {x = 153, z = 128}, [CAVERN] = {x = 103, z = 128}} -- inside each map's portal area
local FIGHT_MAP = {combat_target = CAVERN, combat = CAVERN, combat_guard = MEADOW, death = MEADOW, revive = MEADOW,
                   loot_window = CAVERN, loot_rejects = CAVERN, loot = CAVERN, bag_full = CAVERN,
                   warrior = MEADOW, mage = MEADOW, mage_bolt = MEADOW, priest = MEADOW, rogue = MEADOW,
                   rogue_backstab = CAVERN, equip_rules = MEADOW, equip_combat = MEADOW}
local STAGE_CLASS = {mage = "mage", mage_bolt = "mage", priest = "priest", rogue = "rogue", rogue_backstab = "rogue",
                     legacy = "mage"}
local CLASS = STAGE_CLASS[STAGE] or "warrior"
-- Each step waits `after` seconds, walks to its spot (go m south of the Guard, or "front" / "behind" the Bat), waits for
-- its condition (bat: a living Bat; near: the Guard within that many m; log: a combat log line), then does the rest.
local CLASS_STEPS = {
    warrior = {
        {go = 8}, {tab = true}, {press = 1},                          -- * Out of range
        {go = 2.2, press = 3}, {after = 0.3, press = 1},              -- Shield Wall, then the GCD refuses Strike
        {after = 1.0, go = 2.2, press = 1}, {after = 0.3, press = 1}, -- Strike 12, then its own cooldown refuses it
        {after = 1.0, go = 2.2, press = 2},                           -- Heroic Slam 25
        {log = "Guard hits you for 2", shot = "shield wall"},
    },
    mage = {
        {go = 15}, {tab = true}, {press = 1},                         -- Firebolt 14: the Guard comes running
        {after = 1.1, press = 3},                                     -- Arcane Blast 30
        {after = 1.1, near = 7, press = 2},                           -- Frost Nova roots it
        {log = "You cast Frost Nova: Guard rooted", shot = "frost nova"},
    },
    mage_bolt = {{go = 15}, {tab = true}, {press = 1}, {log = "You cast Firebolt on Guard for 14", shot = "firebolt"}},
    priest = {
        {go = 10}, {tab = true}, {press = 3},                         -- Power Shield
        {after = 1.1, press = 1},                                     -- Smite 10: the Guard comes and hits the shield
        {log = "Power Shield absorbs 5", press = 2},                  -- Heal
        {after = 0.3, shot = "power shield"},
    },
    rogue = {
        {seed = 3}, {go = 2.2}, {tab = true}, {press = 3},            -- Evasion
        {after = 1.1, go = 2.2, press = 1},                           -- Sinister Strike 11
        {log = "You dodge Guard's attack", shot = "evasion"},
    },
    rogue_backstab = {
        {bat = true}, {go = "front"}, {tab = true}, {after = 0.5, press = 2}, -- * Not behind target
        {go = "behind"}, {after = 0.5, press = 2},                    -- Backstab 30: the Bat dies
        {log = "Bat dies", shot = "backstab"},
    },
    -- Equipment (act: see ACTS below): bare, a known bag, then drops replayed as RmlUi publishes them.
    equip_rules = {
        {act = "reset", bag = {"worn_shortsword:1", "linen_tunic:1"}},
        {after = 1.5, act = "equip", from = 2, slot = 1},             -- the tunic on the head: refused by the client
        {after = 0.5, act = "raw_equip", from = 2, slot = 1},         -- ... and by the server (wrong_slot)
        {after = 0.5, act = "equip", from = 1, slot = 6},             -- the sword into Main Hand
        {after = 1, act = "fill"},                                    -- 16 full stacks
        {after = 1, act = "unequip", slot = 6},                       -- * Inventory is full
        {log = "* Inventory is full", go = 2.2}, {tab = true}, {press = 1}, -- Strike: now in combat
        {after = 0.5, act = "unequip", slot = 6},                     -- * Can't do that in combat
        {log = "* Can't do that in combat", shot = "rules"},
    },
    equip_combat = {
        {act = "reset", bag = {"worn_shortsword:1", "linen_tunic:1", "wooden_shield:1"}},
        {after = 1.5, act = "equip", from = 1, slot = 6},             -- +3 Damage
        {after = 0.5, act = "equip", from = 2, slot = 2},             -- 2 Armor, 2 Stamina
        {after = 0.5, act = "equip", from = 3, slot = 7},             -- 2 Armor, 1 Stamina: 155 max Health
        {after = 1, go = 2.2}, {tab = true}, {press = 1},             -- Strike 15
        {log = "Guard hits you for 1", shot = "armored"},             -- 5 - 4 Armor
    },
}
local ROCK_SPOT = {x = 141, z = 103} -- 5 m south of the rock at (141, 108), a 1.2 m cylinder
local SHRINE_SPOT = {x = 118, z = 136.5} -- 2 m from the Meadow Shrine's edge (118, 140): within its range 3
local BAT_SPOT = {x = 128, z = 122.5} -- 2.5 m north of the Bat (128, 120): in reach, the Bat between camera and player
local GUARD_REACH = 2.6 -- m: close enough to hit (the server allows 3)
local ATTACKS_ON_GUARD = 3
local ATTACK_EVERY = 1.7 -- s, over the server's 1.5 s cooldown
local LOOT_STAGES = {loot_window = true, loot_rejects = true, loot = true, bag_full = true}
local SEED = 1 -- test.seed: the Bat drops 1 gold, a Bat Wing and Linen Cloth
local FAR_SPOT = {x = 128, z = 128.5} -- 8.5 m north of the Bat: beyond the 5 m loot range
local FULL_BAG = {}
for i = 1, 16 do FULL_BAG[i] = "linen_cloth:20" end
local SORT_BAG = {"bat_wing:5", "linen_cloth:3", "", "bat_wing:18", "worn_shortsword:1", "", "minor_healing_potion:4"}
local function drop(from, to, shift) return {from = from, to = to, shift = shift} end
local BAG_STEPS = { -- stage -> its drop; each bag stage replays every step up to its own
    {stage = "bag_move", drop = drop(1, 3)},             -- onto an empty slot
    {stage = "bag_swap", drop = drop(2, 3)},             -- Linen Cloth onto the Bat Wings
    {stage = "bag_merge", drop = drop(2, 4)},            -- 5 Bat Wings onto 18: 20 and 3
    {stage = "bag_split", drop = drop(3, 6, true)},      -- Shift: half the Linen Cloth to an empty slot
    {stage = "bag_destroy_confirm", drop = drop(6, "")}, -- dropped on the world: the confirmation dialog
    {stage = "bag_destroy", drop = drop(6, ""), confirm = true},
}
local BAG_STAGE = {}
for i, step in ipairs(BAG_STEPS) do BAG_STAGE[step.stage] = i end
local EQUIP_STAGES = {equip_open = true, equip_drag = true, equip_tooltip = true, equip_persist = true}

local player
local map
local crossings = 0
local target
local elapsed = 0
local chatted = false
local me
local guard, guardId -- the Guard's clone and server id
local targeted, engaged, hits, nextAttack, shot = false, false, 0, 0, false
local bat, batId          -- the latest Bat clone and its server id
local corpses = {}        -- server id -> Corpse.owner
local phase = "arrive"    -- loot and bag stages
local done = 0            -- test.done answers
local bagItems, bagCounts = {}, {}
local bagChanged = false  -- an Inventory update since the last bag step
local gold, goldBefore
local step, waitUntil = 0, 0
local toggles = 0         -- bag.visibility events
local retargetAt = 0      -- combat: when to press Tab again if nothing got selected
local stepIndex, stepAt = 1, 0 -- class stages: the CLASS_STEPS step and when it began
local logged = {}         -- combat log lines seen (class stages wait for some)
local charReplies = 0     -- world.charCreated / charError answers (class_pick)
local equipped = {}       -- the local Equipment, as replicated
local stats               -- the last "character.stats"
local windowToggles = 0   -- character.visibility events

local function log(format, ...) kast.log("test driver: " .. string.format(format, ...)) end

local function attack() kast.publish("combat.power", {id = "power_1"}) end -- as a click on the action bar's first button

local function shoot(why)
    if shot then return end
    shot = true
    log("SHOT %s", why)
end

local function faceNorth() -- before scripts/player.lua's next update puts the camera behind the character
    kast.set(player, "Transform", {rotation = {x = 0, y = 0, z = 0, w = 1}})
end

local function markers()
    local n = 0
    for e in kast.query("MeshRenderer") do
        local m, tf = kast.get(e, "MeshRenderer"), kast.get(e, "Transform")
        if m.material == "materials/gold.mat" and m.primitive == "sphere" and tf.scale.x == 2 then n = n + 1 end
    end
    return n
end

-- Gold spheres of scripts/loot.lua's sparkle size.
local function sparkles()
    local n = 0
    for e in kast.query("MeshRenderer") do
        local m, tf = kast.get(e, "MeshRenderer"), kast.get(e, "Transform")
        if m.material == "materials/gold.mat" and math.abs(tf.scale.x - 0.3) < 0.01 then n = n + 1 end
    end
    return n
end

local function bagText()
    local parts = {}
    for i = 1, 16 do
        if (bagItems[i] or "") ~= "" then parts[#parts + 1] = i .. "=" .. bagItems[i] .. ":" .. bagCounts[i] end
    end
    return table.concat(parts, " ")
end

local function setBag(slots) -- {"item:count" or "", ...}
    local items, counts = {}, {}
    for i = 1, 16 do
        local id, n = string.match(slots[i] or "", "^(.-):(%d+)$")
        items[i], counts[i] = id or "", tonumber(n) or 0
    end
    kast.world.send("test.bag", {items = items, counts = counts})
end

local function gearText()
    local parts = {}
    for i = 1, 7 do
        if (equipped[i] or "") ~= "" then parts[#parts + 1] = i .. "=" .. equipped[i] end
    end
    return table.concat(parts, " ")
end

local function resetGear(slots)
    kast.world.send("test.equipment", {})
    setBag(slots)
end

-- Drags as RmlUi publishes them: bag slot from onto equipment slot; equipment slot onto bag slot to (nil: the bag's
-- frame).
local function dropOnGear(from, slot)
    kast.publish("bag.slot.drop", {id = "slot_" .. from, document = "ui/bag.rml", target = "equip_" .. slot,
        targetDocument = "ui/character.rml", shift = false, ctrl = false, alt = false})
end
local function dropOnBag(slot, to)
    kast.publish("equip.slot.drop", {id = "equip_" .. slot, document = "ui/character.rml",
        target = to and ("slot_" .. to) or "bag", targetDocument = "ui/bag.rml", shift = false, ctrl = false, alt = false})
end

local ACTS = { -- CLASS_STEPS act steps
    reset = function(s) resetGear(s.bag) end,
    equip = function(s) dropOnGear(s.from, s.slot) end,
    raw_equip = function(s) kast.world.send("equipment.equip", {from = s.from, slot = s.slot, item = bagItems[s.from]}) end,
    unequip = function(s) dropOnBag(s.slot, s.to) end,
    fill = function() setBag(FULL_BAG) end,
}

local function batAlive()
    local ok, health = pcall(kast.get, bat, "Health")
    return ok and health.current > 0
end

local function at(spot)
    local p = kast.get(player, "Transform").position
    return math.abs(p.x - spot.x) <= 0.2 and math.abs(p.z - spot.z) <= 0.2
end

-- The loot stages, once on the Cavern: see the header.
local function lootStage()
    if phase == "arrive" then
        target = not at(BAT_SPOT) and BAT_SPOT or nil
        if target then return end
        phase = STAGE == "loot_rejects" and "foreign" or "setup"
    end
    if phase == "foreign" then -- loot_window's corpse, still lying here
        for id, owner in pairs(corpses) do
            if owner ~= me then
                log("foreign corpse %d (owner %d), %d sparkle(s)", id, owner, sparkles())
                kast.publish("loot.clicked", {entityId = id}) -- as a right click on it would
                kast.world.send("loot.open", {target = id})
                kast.world.send("inventory.move", {from = 99, to = 1, item = "bat_wing"})
                kast.world.send("inventory.destroy", {slot = 0, item = ""})
                phase = "setup"
                break
            end
        end
    elseif phase == "setup" then
        setBag(STAGE == "bag_full" and FULL_BAG or {})
        kast.world.send("test.seed", {seed = SEED})
        phase = "setup_wait"
    elseif phase == "setup_wait" then
        if done >= 2 and batAlive() then
            phase = "kill"
            kast.publish("combat.target_next")
            nextAttack = elapsed
        end
    elseif phase == "kill" then
        if not batAlive() then
            phase = "corpse"
        elseif elapsed >= nextAttack then
            nextAttack = elapsed + ATTACK_EVERY
            attack()
        end
    elseif phase == "corpse" then
        if corpses[batId] ~= me then return end
        phase = "corpse_seen"
    elseif phase == "corpse_seen" then -- a frame later: loot.lua made its sparkle
        log("corpse %d owner %d (me), %d sparkle(s)", batId, me, sparkles())
        goldBefore = gold
        if STAGE == "loot_window" then
            log("CLICK")
            phase = "window"
        elseif STAGE == "loot_rejects" then
            target, phase = FAR_SPOT, "far"
        else
            kast.publish("loot.clicked", {entityId = batId})
            phase = "window"
        end
    elseif phase == "far" and not target then
        log("asking from %.1f m", math.sqrt((128 - FAR_SPOT.x) ^ 2 + (120 - FAR_SPOT.z) ^ 2))
        kast.publish("loot.clicked", {entityId = batId})
        kast.world.send("loot.open", {target = batId})
        target, phase = BAT_SPOT, "back"
    elseif phase == "back" and not target then
        kast.world.send("loot.take_all", {target = batId})
        phase = "window"
    end
end

local cameraBefore
local function cameraRotation()
    for e in kast.query("Camera") do return kast.get(e, "Transform").rotation end
end

-- The bag stages: a known bag, then each step's drop up to this stage's, one Inventory update apart.
local function bagStage()
    if phase == "arrive" then
        setBag(SORT_BAG)
        phase = "setup_wait"
    elseif phase == "setup_wait" then
        if done < 1 then return end
        waitUntil, phase = elapsed + 1, "settle"
    elseif phase == "settle" then
        if elapsed < waitUntil then return end
        log("bag set: %s", bagText())
        kast.publish("bag.toggle")
        phase = "step"
        if STAGE == "bag_drag_mouse" then
            bagChanged, phase = false, "mouse"
            cameraBefore = cameraRotation()
            log("DRAG")
        end
    elseif phase == "mouse" then
        if not bagChanged then return end
        phase = "done"
        local now, was = cameraRotation(), cameraBefore
        local turned = math.abs(now.x - was.x) + math.abs(now.y - was.y) + math.abs(now.z - was.z) + math.abs(now.w - was.w)
        log("camera turned by the drag: %s", tostring(turned > 1e-4))
        return shoot("dragged with the mouse")
    elseif phase == "step" then
        step = step + 1
        local s = BAG_STEPS[step]
        local d = s.drop
        log("drop slot_%d -> %s%s", d.from, d.to == "" and "nowhere" or "slot_" .. d.to, d.shift and " (Shift)" or "")
        bagChanged = false
        kast.publish("bag.slot.drop", {id = "slot_" .. d.from, document = "ui/bag.rml",
            target = d.to == "" and "" or "slot_" .. d.to, targetDocument = d.to == "" and "" or "ui/bag.rml",
            shift = d.shift == true, ctrl = false, alt = false})
        if s.confirm then kast.publish("bag.destroy_confirm") end
        waitUntil, phase = elapsed + 1, "step_wait"
    elseif phase == "step_wait" then
        if not bagChanged and not (BAG_STEPS[step].drop.to == "" and not BAG_STEPS[step].confirm and elapsed >= waitUntil) then
            return
        end
        if step == BAG_STAGE[STAGE] then
            phase = "done"
            return shoot(STAGE)
        end
        phase = "step"
    end
end

-- The equipment stages without CLASS_STEPS: see the header.
local function equipStage()
    if phase == "arrive" and elapsed > 2 then
        if STAGE == "equip_tooltip" then
            kast.publish("bag.toggle")
            phase, waitUntil = "shown", elapsed + 1.5
            return log("HOVER")
        end
        if STAGE == "equip_persist" then
            log("equipment after relog: %s", gearText())
            kast.publish("bag.toggle")
            kast.publish("character.toggle")
            phase, waitUntil = "shown", elapsed + 1.5
            return
        end
        resetGear(STAGE == "equip_drag" and {"worn_shortsword:1", "leather_cap:1"} or {})
        phase = "setup_wait"
    elseif phase == "setup_wait" and done >= 2 then
        waitUntil, phase = elapsed + 1, "settle"
    elseif phase == "settle" and elapsed >= waitUntil then
        if STAGE == "equip_open" then
            phase, waitUntil = "keys", elapsed + 6
            return log("KEYS")
        end
        kast.publish("bag.toggle")
        kast.publish("character.toggle")
        phase, waitUntil = "opened", elapsed + 1
    elseif phase == "opened" and elapsed >= waitUntil then
        cameraBefore = cameraRotation()
        phase = "mouse"
        log("DRAG")
    elseif phase == "mouse" and equipped[6] == "worn_shortsword" and stats and stats.damage == 3 then
        phase = "done"
        local now, was = cameraRotation(), cameraBefore
        local turned = math.abs(now.x - was.x) + math.abs(now.y - was.y) + math.abs(now.z - was.z) + math.abs(now.w - was.w)
        log("camera turned by the drag: %s", tostring(turned > 1e-4))
        shoot("equipped with the mouse")
    elseif phase == "keys" and elapsed >= waitUntil then
        phase = "done"
        log("after C, chat, C: %d character toggle(s), chat field '%s'", windowToggles, kast.gui.value("chat_input") or "?")
        shoot("keys")
    elseif phase == "shown" and elapsed >= waitUntil then
        phase = "done"
        log("hovered %s", tostring(kast.gui.hovered()))
        shoot(STAGE)
    end
end

-- The zone object stages: see the header.
local function zoneStage()
    local spot = STAGE == "rock" and ROCK_SPOT or SHRINE_SPOT
    if phase == "arrive" then
        target = not at(spot) and spot or nil
        if target then return end
        faceNorth()
        phase, waitUntil = "keys", elapsed + 5
        log(STAGE == "rock" and "KEYS" or "PRESS")
    elseif phase == "keys" and elapsed >= waitUntil then
        phase = "done"
        local p = kast.get(player, "Transform").position
        log("stopped at %.2f, %.2f", p.x, p.z)
        if STAGE == "rock" then shoot("at the rock") end
    end
end

-- The combat stages' moves and key presses for this frame.
local function fight()
    local p = kast.get(player, "Transform").position
    if STAGE == "combat_target" or STAGE == "combat" then
        target = (math.abs(p.x - BAT_SPOT.x) > 0.2 or math.abs(p.z - BAT_SPOT.z) > 0.2) and BAT_SPOT or nil
        if target or STAGE ~= "combat" then return end
        if not targeted or (not engaged and elapsed >= retargetAt) then -- again until the Bat's clone is there
            targeted, retargetAt = true, elapsed + 1
            kast.publish("combat.target_next")
        elseif elapsed >= nextAttack and engaged then
            nextAttack = elapsed + ATTACK_EVERY
            attack()
        end
        return
    end
    if not guard or engaged then return end
    local g = kast.get(guard, "Transform").position
    target = {x = g.x, z = g.z - 2} -- from the south, so it stays in front of the camera
    if math.sqrt((g.x - p.x) ^ 2 + (g.z - p.z) ^ 2) <= GUARD_REACH then
        target = nil
        if not targeted then
            targeted = true
            kast.publish("combat.target_next")
        else
            engaged = true
            nextAttack = elapsed + ATTACK_EVERY
            attack()
        end
    end
end

-- Yaw of a clone's Transform rotation (a yaw quaternion; 0 faces +z).
local function heading(entity)
    local q = kast.get(entity, "Transform").rotation
    return 2 * math.atan2(q.y, q.w)
end

-- Where a class step stands: go m south of the Guard, or 2.5 m in front of / behind the Bat.
local function spot(go)
    if type(go) == "number" then
        if not guard then return nil end
        local g = kast.get(guard, "Transform").position
        return {x = g.x, z = g.z - go}
    end
    if not bat then return nil end
    local b, h = kast.get(bat, "Transform").position, heading(bat)
    local k = go == "front" and 2.5 or -2.5
    return {x = b.x + math.sin(h) * k, z = b.z + math.cos(h) * k}
end

local function guardWithin(d)
    if not guard then return false end
    local g, p = kast.get(guard, "Transform").position, kast.get(player, "Transform").position
    return math.sqrt((g.x - p.x) ^ 2 + (g.z - p.z) ^ 2) <= d
end

-- The class stages: one CLASS_STEPS step at a time (see the header).
local function classStage()
    local s = CLASS_STEPS[STAGE][stepIndex]
    if not s or elapsed - stepAt < (s.after or 0) then return end
    if s.go then
        local where = spot(s.go)
        if not where then return end
        local p = kast.get(player, "Transform").position
        if math.sqrt((where.x - p.x) ^ 2 + (where.z - p.z) ^ 2) > 0.5 then
            target = where
            return
        end
        target = nil
    end
    if (s.bat and not batAlive()) or (s.near and not guardWithin(s.near)) or (s.log and not logged[s.log]) then return end
    if s.seed then kast.world.send("test.seed", {seed = s.seed}) end
    if s.act then
        log("act %s", s.act)
        ACTS[s.act](s)
    end
    if s.tab then kast.publish("combat.target_next") end
    if s.press then
        log("press %d", s.press)
        kast.publish("combat.power", {id = "power_" .. s.press})
    end
    if s.shot then shoot(s.shot) end
    stepIndex, stepAt = stepIndex + 1, elapsed
end

local function walk(dt)
    local p = kast.get(player, "Transform").position
    local dx, dz = target.x - p.x, target.z - p.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist < 0.2 then
        target = nil
        return
    end
    local step = math.min(dist, SPEED * dt)
    local x, z = p.x + dx / dist * step, p.z + dz / dist * step
    local hit = kast.physics.raycast({x = x, y = 200, z = z}, {x = 0, y = -1, z = 0}, 400)
    kast.set(player, "Transform", {position = {x = x, y = hit and hit.point.y or p.y, z = z}})
end

return {
    init = function()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        kast.subscribe("auth.realms", function(e)
            for i, realm in ipairs(e.realms) do
                if realm.name == REALM then
                    log("picking realm %s", REALM)
                    return kast.publish("realms.pick", {id = "realm_" .. i})
                end
            end
            kast.error("no realm named " .. REALM)
        end)
        kast.subscribe("world.charList", function(e)
            local names = {}
            for _, character in ipairs(e.characters) do
                names[#names + 1] = string.format("%s (%s)", character.name, character.options.class or "no class")
            end
            log("characters: %s", table.concat(names, ", "))
            if STAGE == "class_pick" then
                if phase == "arrive" then -- Create with no class picked: nothing may go out
                    phase, waitUntil = "unpicked", elapsed + 2
                    kast.gui.setText("char_name", CHARACTER)
                    kast.publish("characters.create")
                    log("pressed Create with no class")
                end
                return
            end
            for i, character in ipairs(e.characters) do
                if character.name == CHARACTER then
                    if STAGE ~= "chars" then
                        log("entering as %s", CHARACTER)
                        kast.publish("characters.pick", {id = "char_" .. i})
                    end
                    return
                end
            end
            log("creating %s as %s", CHARACTER, CLASS)
            kast.gui.setText("char_name", CHARACTER)
            kast.publish("characters.class", {id = "class_" .. CLASS})
            kast.publish("characters.create")
        end)
        kast.subscribe("world.charError", function(e)
            charReplies = charReplies + 1
            log("%s failed: %s", e.request, e.code)
        end)
        kast.subscribe("world.charCreated", function(e)
            charReplies = charReplies + 1
            log("created %s (%s)", e.character.name, tostring(e.character.options.class))
        end)
        kast.subscribe("server.class.powers", function(e)
            local names = {}
            for _, p in ipairs(e.powers) do names[#names + 1] = p.key .. " " .. p.name end
            log("powers of %s: %s", e.class, table.concat(names, ", "))
            if STAGE == "legacy" then shoot("legacy") end
        end)
        kast.subscribe("world.entered", function(e)
            map, me = e.map, e.entityId
            if FIGHT_MAP[STAGE] then faceNorth() end
            log("entered map %d at %.1f, %.1f", e.map, e.position.x, e.position.z)
            if STAGE == "world" then target = {x = 124, z = 128} end -- 4 m west of the spawn, where the bot stands
            if STAGE == "bag_persist" then kast.publish("bag.toggle") end
        end)
        kast.subscribe("world.mapChanged", function(e)
            map = e.map
            if FIGHT_MAP[STAGE] then faceNorth() end
            crossings = crossings + 1
            log("crossed to map %d", e.map)
        end)
        kast.subscribe("world.spawn", function(e)
            log("spawn %s %s template '%s'", tostring(e.kind), tostring(e.name), tostring(e.template))
        end)
        kast.subscribe("world.entitySpawned", function(e)
            log("cloned %s (server id %d)", e.name, e.entityId)
            if e.name == "Guard" then guard, guardId = e.entity, e.entityId end
            if e.name == "Bat" then bat, batId = e.entity, e.entityId end
        end)
        kast.subscribe("world.despawn", function(e)
            log("despawn %d", e.entityId)
            corpses[e.entityId] = nil
            if e.entityId == batId then bat, batId = nil, nil end
            if e.entityId == guardId then guard, guardId = nil, nil end -- its clone is gone (map change, death)
        end)
        kast.subscribe("server.test.done", function(e)
            done = done + 1
            log("test %s done", e.what)
        end)
        kast.subscribe("server.inventory.rejected", function(e) log("rejected %s", e.reason) end)
        kast.subscribe("bag.visibility", function(e)
            toggles = toggles + 1
            log("bag open: %s", tostring(e.open))
            if STAGE == "bag_keys" and e.open then
                log("chat focused: %s, focusedField %s", tostring(kast.gui.focus("chat_input")), tostring(kast.gui.focusedField()))
            end
        end)
        kast.subscribe("server.loot.contents", function(e)
            local items = {}
            for _, entry in ipairs(e.items or {}) do items[#items + 1] = entry.item .. " x" .. entry.count end
            log("loot window: %d gold, %s", e.gold, #items > 0 and table.concat(items, ", ") or "no items")
            if STAGE == "loot_window" then return shoot("loot window") end
            if (STAGE == "loot" or STAGE == "bag_full") and phase == "window" then
                phase = "taking"
                kast.publish("loot.take_all")
            elseif STAGE == "bag_full" and phase == "taking" then
                shoot("bag full")
            end
        end)
        kast.subscribe("server.loot.closed", function(e)
            log("loot closed (corpse %d)", e.target)
            if STAGE == "loot" then -- the bag update came with the take, before this
                phase = "bag"
                kast.publish("bag.toggle")
                shoot("bag after loot")
            elseif STAGE == "loot_rejects" then
                shoot("rejects done")
            end
        end)
        kast.subscribe("character.visibility", function(e)
            windowToggles = windowToggles + 1
            log("character open: %s", tostring(e.open))
            if STAGE == "equip_open" and e.open then
                local shown = false
                for doc in kast.query("UIDocument") do
                    local d = kast.get(doc, "UIDocument")
                    shown = shown or (d.path == "ui/character.rml" and d.visible)
                end
                log("character document visible: %s", tostring(shown))
                log("chat focused: %s, focusedField %s", tostring(kast.gui.focus("chat_input")), tostring(kast.gui.focusedField()))
            end
        end)
        kast.subscribe("character.stats", function(e)
            stats = e
            log("stats: max health %d, armor %d, damage +%d", e.maxHealth, e.armor, e.damage)
        end)
        kast.subscribe("target.selected", function(e)
            log("target selected %s (%d), %d marker(s)", e.name, e.entityId, markers())
            if STAGE == "combat_target" then shoot("target") end
            if STAGE == "combat" and not engaged then -- Escape's event, then Tab's again
                engaged = true
                kast.publish("combat.target_clear")
                kast.publish("combat.target_next")
            end
        end)
        kast.subscribe("target.cleared", function() log("target cleared, %d marker(s)", markers()) end)
        kast.subscribe("chat.local", function(e)
            logged[e.text] = true
            log("combat log: %s", e.text)
            if STAGE == "combat" and e.text == "Bat dies" then shoot("bat dead") end
            if STAGE == "death" and e.text == "You die" then shoot("dead") end
            if STAGE == "revive" and e.text == "You revive" then shoot("revived") end
            if STAGE == "shrine" and e.text:find("The shrine hums", 1, true) then shoot("blessing") end
        end)
        kast.subscribe("server.combat.hit", function(e)
            if e.attacker ~= me or STAGE ~= "combat_guard" then return end
            hits = hits + 1
            if hits >= ATTACKS_ON_GUARD then shoot("guard hit " .. hits .. " times") end
        end)
        kast.subscribe("world.chat", function(e) log("chat %s: %s", e.from, e.text) end)
        kast.subscribe("world.component", function(e)
            if e.name == "Health" then log("Health %d / %d on %d", e.data.current, e.data.max, e.entityId) end
            if e.name == "Class" then log("Class %s on %d", e.data.id, e.entityId) end
            if e.name == "Wallet" then
                log("Wallet %d gold on %d", e.data.gold, e.entityId)
                if e.entityId == me then
                    gold = e.data.gold
                    if goldBefore and gold > goldBefore then log("gold +%d", gold - goldBefore) end
                end
            end
            if e.name == "Corpse" then
                corpses[e.entityId] = e.data.owner
                log("corpse %d owner %d", e.entityId, e.data.owner)
            end
            if e.name == "Equipment" and e.entityId == me then
                for i = 1, 7 do equipped[i] = e.data.items[i] or "" end
                log("equipment %s", gearText())
            end
            if e.name == "Inventory" and e.entityId == me then
                for i = 1, 16 do bagItems[i], bagCounts[i] = e.data.items[i] or "", e.data.counts[i] or 0 end
                bagChanged = true
                log("bag %s", bagText())
            end
        end)
    end,
    update = function(dt)
        if STAGE == "class_pick" then
            elapsed = elapsed + dt
            if phase == "unpicked" and elapsed >= waitUntil then
                log("replies to Create with no class: %d", charReplies)
                phase = "picked"
                kast.publish("characters.class", {id = "class_mage"})
                log("picked mage")
                shoot("class picked")
            end
            return
        end
        if not player or not map then return end
        elapsed = elapsed + dt
        if STAGE == "world" and not chatted and elapsed > 1 then
            chatted = true
            kast.gui.setText("chat_input", "hello from the starter sample")
            kast.publish("chat.send")
        end
        if STAGE == "cavern" then
            local done = crossings >= 2 and map == CAVERN
            target = not done and GATES[map] or nil
        end
        if BAG_STAGE[STAGE] or STAGE == "bag_drag_mouse" then bagStage() end
        if STAGE == "rock" or STAGE == "shrine" then zoneStage() end
        if EQUIP_STAGES[STAGE] then equipStage() end
        if STAGE == "bag_keys" and elapsed > 2 and phase == "arrive" then
            phase, waitUntil = "keys", elapsed + 6
            log("KEYS")
        elseif STAGE == "bag_keys" and phase == "keys" and elapsed >= waitUntil then
            phase = "done"
            log("after B, chat, B: %d bag toggle(s), chat field '%s'", toggles, kast.gui.value("chat_input") or "?")
            shoot("keys")
        end
        if STAGE == "bag_persist" and elapsed > 3 and phase == "arrive" then
            phase = "hover"
            log("bag after relog: %s", bagText())
            log("HOVER")
            waitUntil = elapsed + 1.5
        elseif STAGE == "bag_persist" and phase == "hover" and elapsed >= waitUntil then
            phase = "done"
            log("hovered %s", tostring(kast.gui.hovered()))
            shoot("tooltip")
        end
        if FIGHT_MAP[STAGE] then
            if map ~= FIGHT_MAP[STAGE] then
                target = GATES[map]
            elseif LOOT_STAGES[STAGE] then
                lootStage()
            elseif CLASS_STEPS[STAGE] then
                classStage()
            else
                fight()
                if STAGE == "combat_guard" and engaged and hits < ATTACKS_ON_GUARD and elapsed >= nextAttack then
                    nextAttack = elapsed + ATTACK_EVERY
                    attack()
                end
            end
        end
        if target then walk(dt) end
    end,
}
