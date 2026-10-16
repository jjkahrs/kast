-- Sample server system: NPCs. init creates one NPC named "Guard" at the spawn point (kast.create's default), shown on
-- clients as a clone of the scene entity "Guard" (its template), and gives it the "patrol" machine (fsms/patrol.lua).
-- While "walking", update moves it at SPEED m/s toward the next of WAYPOINTS (offsets from the spawn point), facing
-- where it goes; on arrival it switches to "waiting" for WAIT_SECONDS, then walks to the next one.
--
-- Bandwidth: every kast.set of the Guard's Transform is one EntityMove per tick (20 Hz) to each player that sees it,
-- so a walking NPC costs as much as a walking player; a waiting one costs nothing. With many NPCs in a crowd, move
-- them less often (every other tick, say) and let clients interpolate.
local SPEED = 3 -- m/s, a walk
local WAIT_SECONDS = 2
local WAYPOINTS = { {x = 10, z = 10}, {x = 10, z = -10}, {x = -10, z = -10}, {x = -10, z = 10} }

local guard, home
local target = 1
local waited = 0

return {
    init = function()
        guard = kast.create("Guard", {template = "Guard"})
        home = kast.get(guard, "Transform").position
        kast.add(guard, "StateMachine")
        kast.set(guard, "StateMachine", {machine = "patrol"}) -- enters "walking" now
    end,

    update = function(dt)
        if kast.fsm.state(guard) == "waiting" then
            waited = waited + dt
            if waited >= WAIT_SECONDS then
                waited = 0
                target = target % #WAYPOINTS + 1
                kast.fsm.change(guard, "walking")
            end
            return
        end
        local pos = kast.get(guard, "Transform").position
        local goal = WAYPOINTS[target]
        local dx, dz = home.x + goal.x - pos.x, home.z + goal.z - pos.z
        local dist = math.sqrt(dx * dx + dz * dz)
        local step = SPEED * dt
        if dist <= step then
            kast.set(guard, "Transform", {position = {x = pos.x + dx, y = pos.y, z = pos.z + dz}})
            kast.fsm.change(guard, "waiting")
        else
            kast.set(guard, "Transform", {
                position = {x = pos.x + dx / dist * step, y = pos.y, z = pos.z + dz / dist * step},
                heading = math.atan2(dx, dz), -- the engine's heading: 0 faces +z
            })
        end
    end,
}
