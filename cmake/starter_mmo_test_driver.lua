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
-- "SHOT" log lines are for --screenshot-on-log: the moment a stage's screenshot should show.
local STAGE, CHARACTER = "@STAGE@", "@CHARACTER@"
local REALM = "Starter Realm"
local SPEED = 5 -- m/s, under the world server's 10 m/s limit
local MEADOW, CAVERN = 10, 11
local GATES = {[MEADOW] = {x = 153, z = 128}, [CAVERN] = {x = 103, z = 128}} -- inside each map's portal area
local FIGHT_MAP = {combat_target = CAVERN, combat = CAVERN, combat_guard = MEADOW, death = MEADOW, revive = MEADOW}
local BAT_SPOT = {x = 128, z = 122.5} -- 2.5 m north of the Bat (128, 120): in reach, the Bat between camera and player
local GUARD_REACH = 2.6 -- m: close enough to hit (the server allows 3)
local ATTACKS_ON_GUARD = 3
local ATTACK_EVERY = 1.7 -- s, over the server's 1.5 s cooldown

local player
local map
local crossings = 0
local target
local elapsed = 0
local chatted = false
local me
local guard -- the Guard's clone
local targeted, engaged, hits, nextAttack, shot = false, false, 0, 0, false

local function log(format, ...) kast.log("test driver: " .. string.format(format, ...)) end

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

-- The combat stages' moves and key presses for this frame.
local function fight()
    local p = kast.get(player, "Transform").position
    if STAGE == "combat_target" or STAGE == "combat" then
        target = (math.abs(p.x - BAT_SPOT.x) > 0.2 or math.abs(p.z - BAT_SPOT.z) > 0.2) and BAT_SPOT or nil
        if target or STAGE ~= "combat" then return end
        if not targeted then
            targeted = true
            kast.publish("combat.target_next")
        elseif elapsed >= nextAttack and engaged then
            nextAttack = elapsed + ATTACK_EVERY
            kast.publish("combat.attack_target")
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
            kast.publish("combat.attack_target")
        end
    end
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
            for _, character in ipairs(e.characters) do names[#names + 1] = character.name end
            log("characters: %s", table.concat(names, ", "))
            for i, character in ipairs(e.characters) do
                if character.name == CHARACTER then
                    if STAGE ~= "chars" then
                        log("entering as %s", CHARACTER)
                        kast.publish("characters.pick", {id = "char_" .. i})
                    end
                    return
                end
            end
            log("creating %s", CHARACTER)
            kast.gui.setText("char_name", CHARACTER)
            kast.publish("characters.create")
        end)
        kast.subscribe("world.charError", function(e) log("%s failed: %s", e.request, e.code) end)
        kast.subscribe("world.entered", function(e)
            map, me = e.map, e.entityId
            if FIGHT_MAP[STAGE] then faceNorth() end
            log("entered map %d at %.1f, %.1f", e.map, e.position.x, e.position.z)
            if STAGE == "world" then target = {x = 124, z = 128} end -- 4 m west of the spawn, where the bot stands
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
            if e.name == "Guard" then guard = e.entity end
        end)
        kast.subscribe("world.despawn", function(e) log("despawn %d", e.entityId) end)
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
            log("combat log: %s", e.text)
            if STAGE == "combat" and e.text == "Bat dies" then shoot("bat dead") end
            if STAGE == "death" and e.text == "You die" then shoot("dead") end
            if STAGE == "revive" and e.text == "You revive" then shoot("revived") end
        end)
        kast.subscribe("server.combat.hit", function(e)
            if e.attacker ~= me or STAGE ~= "combat_guard" then return end
            hits = hits + 1
            if hits >= ATTACKS_ON_GUARD then shoot("guard hit " .. hits .. " times") end
        end)
        kast.subscribe("world.chat", function(e) log("chat %s: %s", e.from, e.text) end)
        kast.subscribe("world.component", function(e)
            if e.name == "Health" then log("Health %d / %d on %d", e.data.current, e.data.max, e.entityId) end
            if e.name == "Wallet" then log("Wallet %d gold on %d", e.data.gold, e.entityId) end
        end)
    end,
    update = function(dt)
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
        if FIGHT_MAP[STAGE] then
            if map ~= FIGHT_MAP[STAGE] then
                target = GATES[map]
            else
                fight()
                if STAGE == "combat_guard" and engaged and hits < ATTACKS_ON_GUARD and elapsed >= nextAttack then
                    nextAttack = elapsed + ATTACK_EVERY
                    kast.publish("combat.attack_target")
                end
            end
        end
        if target then walk(dt) end
    end,
}
