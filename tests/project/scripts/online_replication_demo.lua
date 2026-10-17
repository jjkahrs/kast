-- Online Services replication sample (docs/engine/online-services.html#replication): logs in, enters the world as "Demo"
-- and walks the NetworkLocalPlayer with WASD. Replication (no code here) sends its moves, applies corrections and shows
-- every remote player as a clone of "Remote Player", with the Lua components the server replicates. Needs a project
-- with "online" enabled; the tests run it against kast_net_test_server's fake auth and fake world (realm Testgrove).
local EMAIL, PASSWORD = "demo@example.com", "demo-password-123"
local REALM, CHARACTER = "Testgrove", "Demo"
local SPEED = 5 -- m/s, under the world server's 10 m/s limit

local player
return {
    init = function()
        if not kast.world then
            return kast.error("kast.world is nil: enable Online Services in project.kast")
        end
        for e in kast.query("NetworkLocalPlayer") do player = e end
        kast.subscribe("auth.login.ok", function() kast.auth.realms() end)
        kast.subscribe("auth.realms", function(e)
            for _, realm in ipairs(e.realms) do
                if realm.name == REALM then return kast.world.connect(realm.id) end
            end
            kast.error("no realm named " .. REALM)
        end)
        kast.subscribe("world.connected", function() kast.world.listCharacters() end)
        kast.subscribe("world.charList", function(e)
            for _, character in ipairs(e.characters) do
                if character.name == CHARACTER then return kast.world.enterWorld(character.id) end
            end
            kast.world.createCharacter(CHARACTER)
        end)
        kast.subscribe("world.charCreated", function(e) kast.world.enterWorld(e.character.id) end)
        kast.subscribe("world.entered", function(e)
            local p = kast.get(player, "Transform").position
            kast.log(string.format("replication: entered, player at %g, %g, %g", p.x, p.y, p.z))
        end)
        kast.subscribe("world.entitySpawned", function(e)
            local p = kast.get(e.entity, "Transform").position
            kast.log(string.format("replication: %s spawned as entity %d (server id %d) at %g, %g, %g", e.name,
                                   e.entity, kast.get(e.entity, "NetworkEntity").entityId, p.x, p.y, p.z))
        end)
        kast.subscribe("world.despawn", function(e) kast.log("replication: server id " .. e.entityId .. " left") end)
        -- Lua components the world server replicates (e.g. its sample Health) are applied to the clone or the player
        -- before this runs; components/health.lua is the server's own definition file.
        kast.subscribe("world.component", function(e)
            kast.log(string.format("replication: server id %d %s = %s", e.entityId, e.name, tostring(e.data.current)))
        end)
        kast.subscribe("world.correction", function(e)
            kast.log(string.format("replication: corrected to %g, %g, %g", e.position.x, e.position.y, e.position.z))
        end)
        kast.subscribe("auth.error", function(e) kast.error(string.format("%s failed: %s", e.op, e.code)) end)
        kast.auth.login(EMAIL, PASSWORD)
    end,
    update = function(dt)
        if not player or kast.world.state() ~= "inWorld" then return end
        local dx, dz = 0, 0
        if kast.input.keyDown("W") then dz = dz - 1 end
        if kast.input.keyDown("S") then dz = dz + 1 end
        if kast.input.keyDown("A") then dx = dx - 1 end
        if kast.input.keyDown("D") then dx = dx + 1 end
        if dx == 0 and dz == 0 then return end
        local p = kast.get(player, "Transform").position
        kast.set(player, "Transform", {position = {x = p.x + dx * SPEED * dt, y = p.y, z = p.z + dz * SPEED * dt}})
    end,
}
