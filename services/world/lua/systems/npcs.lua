-- Sample server system: NPC patrols. The NPCs themselves come from the maps' spawn files (README "NPC spawns"); this
-- walks every one whose Spawn has a path. Each gets the "patrol" machine (fsms/patrol.lua) the first time it is seen.
-- While "walking", update moves it at SPEED m/s toward its next path point, facing where it goes; on arrival it
-- switches to "waiting" for the path's wait seconds, then walks to the next point: back to the first after the last
-- (loop), or back along the path (loop = false). A respawned NPC is a new entity and starts over at the first point.
-- On a map with a navmesh (README "Navmesh") it walks kast.map.path's corners from where it is to the next point, so it
-- goes around cliffs; without one (or no path), straight there. The server keeps it on the ground.
--
-- Bandwidth: every kast.set of an NPC's Transform is one EntityMove per tick (20 Hz) to each player that sees it, so a
-- walking NPC costs as much as a walking player; a waiting one costs nothing. With many NPCs in a crowd, move them less
-- often (every other tick, say) and let clients interpolate.
local SPEED = 3 -- m/s, a walk

local patrols = {} -- entity -> {target = point index, step = +1 or -1, waited = seconds, route = corners, leg = index}

-- The point after target, and the direction to go on in.
local function advance(spawn, p)
    local n = #spawn.path
    if spawn.loop then return p.target % n + 1, 1 end
    local step = p.step
    if p.target + step < 1 or p.target + step > n then step = -step end
    return math.max(1, math.min(n, p.target + step)), step
end

local function walk(e, spawn, p, dt)
    if kast.fsm.state(e) == "waiting" then
        p.waited = p.waited + dt
        if p.waited >= spawn.wait then
            p.waited = 0
            p.target, p.step = advance(spawn, p)
            p.route = nil
            kast.fsm.change(e, "walking")
        end
        return
    end
    local tf = kast.get(e, "Transform")
    local pos = tf.position
    if not p.route then
        local point = spawn.path[p.target] -- spawn files may leave y 0: the path wants it on the ground
        local to = {x = point.x, y = kast.map.ground(tf.map, point.x, point.z) or point.y, z = point.z}
        p.route = kast.map.path(tf.map, pos, to) or {point}
        p.leg = 1
    end
    local goal = p.route[p.leg]
    local dx, dz = goal.x - pos.x, goal.z - pos.z
    local dist = math.sqrt(dx * dx + dz * dz)
    local step = SPEED * dt
    if dist <= step then
        kast.set(e, "Transform", {position = {x = goal.x, y = pos.y, z = goal.z}})
        p.leg = p.leg + 1
        if p.leg > #p.route then
            p.route = nil
            kast.fsm.change(e, "waiting")
        end
    else
        kast.set(e, "Transform", {
            position = {x = pos.x + dx / dist * step, y = pos.y, z = pos.z + dz / dist * step},
            heading = math.atan2(dx, dz), -- the engine's heading: 0 faces +z
        })
    end
end

return {
    update = function(dt)
        local live = {}
        for e in kast.query("Spawn") do
            local spawn = kast.get(e, "Spawn")
            if spawn.path then
                local p = patrols[e]
                if not p then
                    p = {target = 1, step = 1, waited = 0}
                    kast.add(e, "StateMachine")
                    kast.set(e, "StateMachine", {machine = "patrol"}) -- enters "walking" now
                end
                live[e] = p
                walk(e, spawn, p, dt)
            end
        end
        patrols = live -- drops the NPCs that are gone
    end,
}
