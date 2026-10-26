-- TEST ONLY (ctest starter_mmo_nameplates*, cmake/starter_mmo_nameplates.cmake): added to a copy of samples/starter-mmo's
-- meadow.scene, never to the sample itself. Offline, so it plays the world client's part on the bus, as the engine's
-- world client and replication publish it: "server.welcome" and "world.entered" for the local player (Tester, 140
-- Health), "world.spawn" for a Guard, the friendly Elder, a Bat and a remote player (Rival) in front of the camera
-- (replication clones the NPC templates and the Remote Player, their nameplates with them), and "world.component"
-- Health for all but the Elder. At HIT_FRAME the Guard loses 12 Health and the Bat dies; it logs SHOT (for
-- --screenshot-on-log), what scripts/nameplates.lua made of it (the floating text count, the Bat's plate hidden), and
-- the floating text count again a second later; and each change of kast.gui.mouseOver() (the notice board's hover).
-- PERF=true (the 100-nameplate perf run) spawns PERF_COUNT Guards in a grid instead and logs the average frame time
-- over PERF_FRAMES frames after PERF_WARMUP.
local PERF, PERF_COUNT, PERF_WARMUP, PERF_FRAMES = @PERF@, 100, 120, 600
local SPAWN_FRAME, HIT_FRAME = 20, 60
local ME = 1
local SPAWNS = { -- entityId, name, kind, template, x, y (the terrain there), z, Health (none: friendly)
    {id = 9001, name = "Guard", kind = "npc", template = "Guard", x = 126.5, y = 5.86, z = 131, health = {50, 50}},
    {id = 9002, name = "Elder", kind = "npc", template = "Elder", x = 124, y = 5.92, z = 131.5},
    {id = 9003, name = "Bat", kind = "npc", template = "Bat", x = 131.5, y = 5.56, z = 129, health = {40, 40}},
    {id = 9004, name = "Rival", kind = "player", template = "", x = 131, y = 6.39, z = 135.5, health = {100, 140}},
}
local frame, measured, spent = 0, 0, 0
local over = false -- kast.gui.mouseOver(), logged when it changes (the notice board's hover and click runs)
local clones = {} -- entityId -> scene entity

local function log(fmt, ...) kast.log("test nameplates: " .. string.format(fmt, ...)) end

local function health(id, current, max)
    kast.publish("world.component", {entityId = id, name = "Health", data = {current = current, max = max, regen = true}})
end

local function spawn(s)
    kast.publish("world.spawn", {entityId = s.id, name = s.name, kind = s.kind, template = s.template,
                                 position = {x = s.x, y = s.y, z = s.z}, heading = 0})
    if s.health then health(s.id, s.health[1], s.health[2]) end
end

local function perfSpawns() -- a 10 x 10 grid, 2.5 m apart, from just past the player (the camera looks north, +z) on
    for i = 0, PERF_COUNT - 1 do
        local x, z = 116.75 + (i % 10) * 2.5, 131 + math.floor(i / 10) * 2.5
        spawn({id = 10000 + i, name = "Guard " .. (i + 1), kind = "npc", template = "Guard", x = x, y = 5.4, z = z,
               health = {50 - i % 50, 50}})
    end
end

return {
    init = function()
        kast.subscribe("world.entitySpawned", function(e) clones[e.entityId] = e.entity end)
    end,
    update = function(dt)
        frame = frame + 1
        if kast.gui.mouseOver() ~= over then
            over = not over
            log("mouseOver %s", tostring(over))
        end
        if frame == SPAWN_FRAME then
            kast.publish("server.welcome", {name = "Tester"})
            kast.publish("world.entered", {entityId = ME, map = 10, position = {x = 128, y = 5.43, z = 128}, heading = 0})
            health(ME, 140, 140)
            if PERF then perfSpawns() else for _, s in ipairs(SPAWNS) do spawn(s) end end
        elseif PERF then
            if frame == SPAWN_FRAME + 2 then
                local plates = 0
                for _, e in pairs(clones) do
                    if kast.has(e, "WorldUI") then plates = plates + 1 end
                end
                log("perf: %d Guards with nameplates", plates)
            elseif frame > PERF_WARMUP and measured < PERF_FRAMES then
                measured, spent = measured + 1, spent + dt
                if measured == PERF_FRAMES then
                    log("perf: average frame time %.3f ms over %d frames (%.1f fps)", 1000 * spent / measured, measured,
                        measured / spent)
                end
            end
        elseif frame == SPAWN_FRAME + 2 then
            for _, s in ipairs(SPAWNS) do
                local e = clones[s.id]
                local ui = e and kast.has(e, "WorldUI") and kast.get(e, "WorldUI")
                log("%s (%s) plate %s, Health %s", s.name, s.kind, ui and ui.path or "none",
                    e and kast.has(e, "Health") and "yes" or "no")
            end
        elseif frame == HIT_FRAME then
            health(9001, 38, 50)
            health(9003, 0, 40)
        elseif frame == HIT_FRAME + 1 then -- the bus delivered the hits after the last update
            log("hit: %d floating text(s), Bat plate visible %s, Guard plate visible %s", kast.gui.worldTextCount(),
                tostring(kast.get(clones[9003], "WorldUI").visible), tostring(kast.get(clones[9001], "WorldUI").visible))
            log("SHOT")
        elseif frame == HIT_FRAME + 70 then
            log("after 70 frames: %d floating text(s)", kast.gui.worldTextCount())
        end
    end,
}
