-- The local player (NetworkLocalPlayer) and the camera: WASD walks it at SPEED m/s on the terrain, facing where it
-- goes, and plays Walk_Female / Idle_A; the camera follows from behind. Replication sends the moves to the server.
-- Keys typed into a GUI text field (the chat box) don't move it.
local SPEED = 5 -- m/s, under the world server's 10 m/s limit
local CAMERA_OFFSET = {x = 0, y = 7, z = 11}
local CAMERA_PITCH = math.rad(-28)

local player, camera
local walking = false

local function groundY(x, z, fallback)
    local hit = kast.physics.raycast({x = x, y = 200, z = z}, {x = 0, y = -1, z = 0}, 400)
    return hit and hit.point.y or fallback
end

local function animate(moving)
    if moving == walking then return end
    walking = moving
    kast.playAnimation(player, moving and "Walk_Female" or "Idle_A", {loop = true, fade = 0.2})
end

return {
    init = function()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        for e in kast.query("Camera") do camera = e end
    end,
    update = function(dt)
        if not player then return end
        local p = kast.get(player, "Transform").position
        local dx, dz = 0, 0
        if kast.world and kast.world.state() == "inWorld" and not kast.gui.focusedField() then
            if kast.input.keyDown("W") then dz = dz - 1 end
            if kast.input.keyDown("S") then dz = dz + 1 end
            if kast.input.keyDown("A") then dx = dx - 1 end
            if kast.input.keyDown("D") then dx = dx + 1 end
        end
        animate(dx ~= 0 or dz ~= 0)
        if walking then
            local len = math.sqrt(dx * dx + dz * dz)
            local x, z = p.x + dx / len * SPEED * dt, p.z + dz / len * SPEED * dt
            local heading = math.atan2(dx, dz) -- 0 faces +z, as the protocol
            p = {x = x, y = groundY(x, z, p.y), z = z}
            kast.set(player, "Transform", {
                position = p,
                rotation = {x = 0, y = math.sin(heading / 2), z = 0, w = math.cos(heading / 2)},
            })
        end
        if camera then
            kast.set(camera, "Transform", {
                position = {x = p.x + CAMERA_OFFSET.x, y = p.y + CAMERA_OFFSET.y, z = p.z + CAMERA_OFFSET.z},
                rotation = {x = math.sin(CAMERA_PITCH / 2), y = 0, z = 0, w = math.cos(CAMERA_PITCH / 2)},
            })
        end
    end,
}
