-- TEST ONLY (ctest starter_mmo_live_*, cmake/starter_mmo_live_play.cmake): added to a copy of samples/starter-mmo's
-- realms and world scenes, never to the sample itself. It plays the sample's screens the way a player would, by
-- publishing the events their buttons publish (realms.pick, characters.pick / .create, chat.send) and filling text
-- fields with kast.gui.setText, so the sample's own handlers do the work, then logs what arrives for the test to check.
-- Stages: "chars" stops at the character list; "world" enters as CHARACTER, steps beside the idle bot at the spawn and
-- chats; "cavern" enters as CHARACTER and walks through the portals until it has crossed both ways and is in the Cavern.
local STAGE, CHARACTER = "@STAGE@", "@CHARACTER@"
local REALM = "Starter Realm"
local SPEED = 5 -- m/s, like scripts/player.lua
local MEADOW, CAVERN = 10, 11
local GATES = {[MEADOW] = {x = 153, z = 128}, [CAVERN] = {x = 103, z = 128}} -- inside each map's portal area

local player
local map
local crossings = 0
local target
local elapsed = 0
local chatted = false

local function log(format, ...) kast.log("test driver: " .. string.format(format, ...)) end

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
            map = e.map
            log("entered map %d at %.1f, %.1f", e.map, e.position.x, e.position.z)
            if STAGE == "world" then target = {x = 124, z = 128} end -- 4 m west of the spawn, where the bot stands
        end)
        kast.subscribe("world.mapChanged", function(e)
            map = e.map
            crossings = crossings + 1
            log("crossed to map %d", e.map)
        end)
        kast.subscribe("world.spawn", function(e)
            log("spawn %s %s template '%s'", tostring(e.kind), tostring(e.name), tostring(e.template))
        end)
        kast.subscribe("world.entitySpawned", function(e) log("cloned %s (server id %d)", e.name, e.entityId) end)
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
        if target then walk(dt) end
    end,
}
