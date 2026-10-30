-- Gamepad play test (the gamepad_* tests, --sim-gamepad): project.kast's "move" and "turn" axes (left stick, W/S, A/D)
-- run and turn the Player cube, "lookX" (right stick) orbits the camera around it, as samples/starter-mmo's
-- scripts/player.lua does. On frame LOG_FRAME it logs where the cube is and the camera's yaw, for the tests to check.
local SPEED = 3 -- m/s
local TURN_RATE = math.rad(90) -- per second, "turn" fully pushed
local LOOK_RATE = math.rad(90) -- per second, "lookX" fully pushed
local DISTANCE, HEIGHT, PITCH = 6, 2.5, math.rad(-20)
local LOG_FRAME = 100

local player, camera
local heading, yaw, frame = 0, 0, 0 -- heading 0 faces -z; yaw 0: the camera behind, on +z

local function yawPitch(y, p) -- yaw about +y after pitch about +x
    local sy, cy, sx, cx = math.sin(y / 2), math.cos(y / 2), math.sin(p / 2), math.cos(p / 2)
    return {x = cy * sx, y = sy * cx, z = -sy * sx, w = cy * cx}
end

return {
    init = function()
        for e in kast.query("MeshRenderer") do
            if kast.get(e, "MeshRenderer").primitive == "cube" then player = e end
        end
        for e in kast.query("Camera") do camera = e end
    end,
    update = function(dt)
        frame = frame + 1
        local turn = kast.input.axis("turn") * TURN_RATE * dt
        heading = heading - turn
        yaw = yaw - turn - kast.input.axis("lookX") * LOOK_RATE * dt
        local p = kast.get(player, "Transform").position
        local forward = kast.input.axis("move") * SPEED * dt
        p.x, p.z = p.x - math.sin(heading) * forward, p.z - math.cos(heading) * forward
        kast.set(player, "Transform", {position = p, rotation = yawPitch(heading, 0)})
        kast.set(camera, "Transform", {
            position = {x = p.x + math.sin(yaw) * DISTANCE, y = HEIGHT, z = p.z + math.cos(yaw) * DISTANCE},
            rotation = yawPitch(yaw, PITCH),
        })
        if frame == LOG_FRAME then
            kast.log(string.format("gamepad test: pad %s, position x=%.2f z=%.2f, camera yaw=%.2f",
                                   kast.input.gamepadConnected() and "connected" or "none", p.x, p.z, yaw))
        end
    end,
}
