-- TEST ONLY (ctest starter_mmo_hit_sparks, cmake/starter_mmo_hit_sparks.cmake): added to a copy of samples/starter-mmo's
-- meadow.scene, never to the sample itself. Offline, so it plays the server's part: it makes a stand-in NPC in front of
-- the player, announces it as world.entitySpawned would and publishes "server.combat.hit" on it, as the world client
-- does for the server's combat.hit. scripts/combat.lua then throws its spark burst; the driver logs the new emitter's
-- alive count until it is destroyed, hits again and logs HITSHOT (for --screenshot-on-log) while the second burst flies.
local NPC_ID = 9001
local frame, before, dummy, hits, watching = 0, nil, nil, 0, false

local function log(fmt, ...) kast.log("test hit: " .. string.format(fmt, ...)) end

local function emitters()
    local set = {}
    for e in kast.query("ParticleEmitter") do set[e] = true end
    return set
end

local function hit()
    hits = hits + 1
    kast.publish("server.combat.hit", {attacker = NPC_ID + 1, target = NPC_ID, attackerName = "Tester",
                                       targetName = "Dummy", damage = 12})
end

return {
    update = function(dt)
        frame = frame + 1
        if frame == 30 then
            before = emitters()
            dummy = kast.create("Hit Dummy")
            kast.set(dummy, "Transform", {position = {x = 128.5, y = 5.8, z = 131}})
            kast.publish("world.entitySpawned", {entityId = NPC_ID, entity = dummy, name = "Dummy", kind = "npc",
                                                 template = "Guard"})
            hit()
            watching = true
        elseif watching then
            local sparks = 0
            for e in pairs(emitters()) do
                if not before[e] then
                    sparks = sparks + 1
                    log("hit %d: sparks %d alive", hits, kast.particles.alive(e))
                end
            end
            if hits == 1 and sparks == 0 and frame > 32 then
                log("hit 1: sparks destroyed")
                hit()
            elseif hits == 2 and sparks > 0 then
                log("HITSHOT")
                watching = false
            end
        end
    end,
}
