-- The local player (NetworkLocalPlayer) and the camera, with World of Warcraft's default third-person controls:
-- W runs forward, S backpedals, A/D turn (strafe while the right mouse button is held), Space jumps. Left drag orbits
-- the camera around the character; right drag turns the camera and the character with it; the wheel zooms. Running
-- forward swings the camera back behind. The cursor hides while dragging and comes back where the drag began.
-- A press that starts on the GUI (chat box, HUD) turns nothing, and keys typed into a GUI text field don't move.
-- Zone obstacles (the meadow's rocks, trees and shrine: static colliders, maps/meadow/objects) stop the character; it
-- slides along them when it can. The world server cuts the same shapes from its navmesh.
-- Replication sends the moves to the server.
-- Combat (systems/combat.lua on the server): the character plays ATTACK_CLIP as soon as one of its powers is used
-- (scripts/powers.lua's "power.predicted"), stopped if the server refuses it ("power.rejected"), and HIT_CLIP when it
-- is hit, while standing still; dead, it plays DEATH_CLIP and does not move or jump (the
-- server refuses its moves anyway) until it revives. The camera still works.
local RUN_SPEED = 7 -- m/s, forward and strafing (the world server allows 10)
local BACK_SPEED = 4.5 -- m/s, backpedalling
local TURN_RATE = math.rad(180) -- per second, A/D without the right button
local JUMP_HEIGHT = 1.6 -- m
local GRAVITY = 20 -- m/s^2
local ZOOM_MIN, ZOOM_MAX, ZOOM_START = 2, 30, 10 -- m from the pivot
local ZOOM_STEP = 1.15 -- distance factor per wheel notch
local PITCH_MIN, PITCH_MAX, PITCH_START = math.rad(-85), math.rad(85), math.rad(-20) -- negative looks down
local MOUSE_SENSITIVITY = math.rad(0.25) -- per pixel dragged
local FOLLOW_RATE = 4 -- 1/s: how fast the camera swings back behind while running forward
local PIVOT_HEIGHT = 1.5 -- m above the feet: head and shoulders
local CAMERA_CLEARANCE = 0.3 -- m kept above the terrain
local BODY_RADIUS = 0.45 -- m: how close the character's middle gets to an obstacle
local FADE = 0.2 -- s, animation crossfade
local JUMP_ANIM_SPEED = 2.5 -- Jump_Start and Jump_Land are longer than a jump
local ATTACK_CLIP, HIT_CLIP, DEATH_CLIP = "Punch_Jab", "Hit_Chest", "Death_A" -- no weapon model: a punch

local player, camera
local yaw, pitch, distance = 0, PITCH_START, ZOOM_START -- camera; yaw 0 looks along -z
local snapBehind = true -- put the camera behind the character on the next update (entering, map change)
local press -- nil, "camera" or "gui": what the held mouse buttons started on
local vy, airborne = 0, false
local clip, jumpPhase -- the playing clip; nil, "start", "air" or "land"
local me -- the character's server entity id
local action -- ATTACK_CLIP or HIT_CLIP waiting to play or playing, until it ends or the character moves
local dead = false

local function groundY(x, z, fallback)
    local hit = kast.physics.raycast({x = x, y = 200, z = z}, {x = 0, y = -1, z = 0}, 400)
    return hit and hit.point.y or fallback
end

-- Whether the character, standing at (x, feet, z), would touch an obstacle: terrain is the scene's only other collider.
local function blocked(x, z, feet)
    for _, e in ipairs(kast.physics.overlapSphere({x = x, y = feet + 1, z = z}, BODY_RADIUS)) do
        if not kast.has(e, "TerrainTile") then return true end
    end
    return false
end

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function wrap(a) return (a + math.pi) % (2 * math.pi) - math.pi end -- to -pi..pi

local function yawPitch(yawAngle, pitchAngle) -- rotation: yaw about +y after pitch about +x
    local sy, cy = math.sin(yawAngle / 2), math.cos(yawAngle / 2)
    local sx, cx = math.sin(pitchAngle / 2), math.cos(pitchAngle / 2)
    return {x = cy * sx, y = sy * cx, z = -sy * sx, w = cy * cx}
end

local function play(name, loop, speed)
    if name == clip then return end
    clip = name
    kast.playAnimation(player, name, {loop = loop, speed = speed or 1, fade = FADE})
end

local function clipDone() return not kast.getAnimationState(player).playing end

-- Which held buttons drive the camera: a press on the GUI, or outside the world, drives nothing until released.
local function mouseButtons(inWorld)
    local left, right = kast.input.mouseDown("left"), kast.input.mouseDown("right")
    if not left and not right then
        press = nil
    elseif not press then
        press = (inWorld and not (kast.gui and kast.gui.mouseOver())) and "camera" or "gui"
    end
    local dragging = press == "camera"
    kast.input.setCursorLocked(dragging)
    return dragging and left, dragging and right
end

local function animate(forward, strafe, jumped, landed)
    if dead then return play(DEATH_CLIP, false) end
    if jumped then
        jumpPhase = "start"
        return play("Jump_Start", false, JUMP_ANIM_SPEED)
    end
    local moving = forward ~= 0 or strafe ~= 0
    if airborne then
        if jumpPhase ~= "air" and (jumpPhase ~= "start" or clipDone()) then
            jumpPhase = "air"
            play("Jump_air", true)
        end
        return
    end
    if landed then jumpPhase = "land" end
    if jumpPhase == "land" and not moving and (clip ~= "Jump_Land" or not clipDone()) then
        return play("Jump_Land", false, JUMP_ANIM_SPEED)
    end
    jumpPhase = nil
    if action and not moving then
        if clip ~= action then
            clip = nil -- the same clip again restarts it
            return play(action, false)
        end
        if not clipDone() then return end
    end
    action = nil
    if forward > 0 then play("Run_Female", true)
    elseif forward < 0 then play("Walk_Backwards", true)
    elseif strafe > 0 then play("Strafe_right", true)
    elseif strafe < 0 then play("Strafe_left", true)
    else play("Idle_A", true) end
end

local function placeCamera(p)
    local pivot = {x = p.x, y = p.y + PIVOT_HEIGHT, z = p.z}
    local horizontal = math.cos(pitch) * distance
    local c = {
        x = pivot.x + math.sin(yaw) * horizontal,
        y = pivot.y - math.sin(pitch) * distance,
        z = pivot.z + math.cos(yaw) * horizontal,
    }
    c.y = math.max(c.y, groundY(c.x, c.z, -math.huge) + CAMERA_CLEARANCE) -- raised over a hill: still looks at pivot
    local dx, dy, dz = pivot.x - c.x, pivot.y - c.y, pivot.z - c.z
    local lookPitch = math.atan2(dy, math.sqrt(dx * dx + dz * dz))
    kast.set(camera, "Transform", {position = c, rotation = yawPitch(math.atan2(-dx, -dz), lookPitch)})
end

return {
    init = function()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        for e in kast.query("Camera") do camera = e end
        kast.subscribe("world.entered", function(e)
            snapBehind, me, dead, action = true, e.entityId, false, nil
        end)
        kast.subscribe("power.predicted", function() action = ATTACK_CLIP end)
        kast.subscribe("power.rejected", function(e) if e.predicted and action == ATTACK_CLIP then action = nil end end)
        kast.subscribe("server.combat.hit", function(e)
            if e.target == me and action ~= ATTACK_CLIP then action = HIT_CLIP end
        end)
        kast.subscribe("server.combat.death", function(e) if e.entity == me then dead = true end end)
        kast.subscribe("server.combat.revive", function(e) if e.entity == me then dead, action = false, nil end end)
        kast.subscribe("world.mapChanged", function() snapBehind = true end)
    end,
    update = function(dt)
        if not player then return end
        local transform = kast.get(player, "Transform")
        local p = transform.position
        local heading = 2 * math.atan2(transform.rotation.y, transform.rotation.w) -- 0 faces +z, as the protocol
        local startHeading = heading
        if snapBehind then
            yaw, pitch, distance, snapBehind = heading + math.pi, PITCH_START, ZOOM_START, false
        end

        local inWorld = kast.world and kast.world.state() == "inWorld"
        local orbit, steer = mouseButtons(inWorld)
        if orbit or steer then
            local mx, my = kast.input.mouseDelta()
            yaw = yaw - mx * MOUSE_SENSITIVITY
            pitch = clamp(pitch - my * MOUSE_SENSITIVITY, PITCH_MIN, PITCH_MAX)
        end
        local wheel = kast.input.wheelDelta()
        if inWorld and wheel ~= 0 and not (kast.gui and kast.gui.mouseOver()) then
            distance = clamp(distance * ZOOM_STEP ^ -wheel, ZOOM_MIN, ZOOM_MAX)
        end

        local keys = inWorld and not dead and not (kast.gui and kast.gui.focusedField())
        local function key(name) return keys and kast.input.keyDown(name) end
        local forward = (key("W") and 1 or 0) - (key("S") and 1 or 0)
        local side = (key("D") and 1 or 0) - (key("A") and 1 or 0)
        local strafe = 0
        if steer then
            heading = yaw - math.pi -- face where the camera looks
            strafe = side
        elseif side ~= 0 then
            local turn = -side * TURN_RATE * dt -- D turns right: clockwise seen from above
            heading = heading + turn
            if not orbit then yaw = yaw + turn end -- the camera turns with the character
        end
        if forward > 0 and not orbit and not steer then -- swing back behind
            yaw = yaw + wrap(heading + math.pi - yaw) * math.min(1, FOLLOW_RATE * dt)
        end

        local fx, fz = math.sin(heading), math.cos(heading)
        local dx, dz = fx * forward - fz * strafe, fz * forward + fx * strafe -- right is (-fz, fx)
        local len = math.sqrt(dx * dx + dz * dz)
        local x, z = p.x, p.z
        if len > 0 then
            local speed = forward < 0 and BACK_SPEED or RUN_SPEED
            x, z = x + dx / len * speed * dt, z + dz / len * speed * dt
            if blocked(x, z, p.y) then
                -- Up to the obstacle (the step's longest free part, to 1/64 of it), so where the character stops
                -- doesn't depend on the frame rate; then slide the rest along it on one axis, else stop there.
                local free, hit = 0, 1
                for _ = 1, 6 do
                    local mid = (free + hit) / 2
                    if blocked(p.x + (x - p.x) * mid, p.z + (z - p.z) * mid, p.y) then hit = mid else free = mid end
                end
                local cx, cz = p.x + (x - p.x) * free, p.z + (z - p.z) * free
                if not blocked(x, cz, p.y) then z = cz elseif not blocked(cx, z, p.y) then x = cx else x, z = cx, cz end
            end
        end
        local ground = groundY(x, z, p.y)
        local y, jumped, landed = ground, false, false
        if airborne then
            vy = vy - GRAVITY * dt
            y = p.y + vy * dt
            if y <= ground then
                y, airborne, landed = ground, false, true
            end
        elseif key("Space") then
            airborne, jumped, vy, y = true, true, math.sqrt(2 * GRAVITY * JUMP_HEIGHT), ground
        end
        animate(forward, strafe, jumped, landed)

        if x ~= p.x or y ~= p.y or z ~= p.z or math.abs(wrap(heading - startHeading)) > 1e-4 then
            p = {x = x, y = y, z = z}
            kast.set(player, "Transform", {
                position = p,
                rotation = {x = 0, y = math.sin(heading / 2), z = 0, w = math.cos(heading / 2)},
            })
        end
        if camera then placeCamera(p) end
    end,
}
