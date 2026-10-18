-- Tab-target combat, client side. The server (systems/combat.lua) decides every hit; this only picks a target, asks
-- for attacks and shows what the server sends back.
-- Targeting: Tab cycles the NPCs in view, nearest first, within TAB_RANGE; a left click (press and release without
-- dragging, not on the GUI) on an NPC selects it (kast.input.mouseRay against each NPC's PICK sphere, unless the
-- terrain is closer); Escape clears. The target clears when its NPC despawns or dies (or the map changes). A gold disc
-- (MARKER_*) under the target shows it. "target.selected" {entityId, name, health} / "target.cleared" go on the bus for the
-- HUD's target frame (scripts/hud.lua).
-- 1 asks the server to attack the target: kast.world.send("combat.attack", {target = <entity id>}). Keys typed into a
-- GUI text field do nothing, as in scripts/player.lua, and neither do they while the local player is dead. The keys'
-- actions are also bus events, "combat.target_next", "combat.target_clear" and "combat.attack_target", for buttons
-- (and tests) to publish.
-- The server's combat.* events become combat log lines in the chat box ("chat.local", scripts/chat.lua), and other
-- players' clones play the attack, hit and death clips (the local player's are scripts/player.lua's).
--
-- Bandwidth: one ClientEvent per key press (the server takes 10 a second at most); everything else is local.
local TAB_RANGE = 40 -- m
local CLICK_SLOP = 4 -- pixels of mouse travel a click may have; more is a camera drag
local PICK = {Guard = {y = 1.1, radius = 1.2}, Bat = {y = 2.25, radius = 1.0}} -- by template: sphere over the feet
local PICK_DEFAULT = {y = 1, radius = 1}
local MARKER_SCALE = {x = 2, y = 0.06, z = 2}
local MARKER_MATERIAL = "materials/gold.mat"
local ATTACK_CLIP, HIT_CLIP, DEATH_CLIP, IDLE_CLIP = "Punch_Jab", "Hit_Chest", "Death_A", "Idle_A"
local REJECTED = {
    range = "Out of range",
    cooldown = "Not ready yet",
    target = "Invalid target",
    target_dead = "Your target is dead",
    dead = "You are dead",
}

local me                -- the local player's server entity id
local dead = false      -- the local player is dead: no attacks
local player            -- the local player's scene entity
local npcIds = {}       -- server ids of the NPCs in view (world.spawn kind "npc") -> template
local clones = {}       -- server id -> {entity = clone, name =, template = (NPCs)}
local target, marker    -- the target's server id, the marker entity under its clone
local held = {}         -- key or button name -> down last frame
local click             -- the left press being watched: {origin, dir, travel}
local oneShots = {}     -- remote player clone -> true while an attack or hit clip plays

local function log(text) kast.publish("chat.local", {text = text}) end

-- True on the frame name goes down.
local function pressed(name, down)
    local was = held[name]
    held[name] = down
    return down and not was
end

local function clearTarget()
    if not target then return end
    target = nil
    if marker then pcall(kast.destroy, marker) end -- gone already if its clone was
    marker = nil
    kast.publish("target.cleared", {})
end

local function selectTarget(id)
    if id == target then return end
    clearTarget()
    local npc = clones[id]
    target = id
    marker = kast.create("Target Marker", npc.entity)
    kast.set(marker, "Transform", {position = {x = 0, y = 0.03, z = 0}, scale = MARKER_SCALE})
    kast.add(marker, "MeshRenderer")
    kast.set(marker, "MeshRenderer", {primitive = "sphere", material = MARKER_MATERIAL, castShadows = false})
    local health = kast.has(npc.entity, "Health") and kast.get(npc.entity, "Health") or nil -- replicated
    kast.publish("target.selected", {entityId = id, name = npc.name, health = health})
end

local function position(entity) return kast.get(entity, "Transform").position end

local function distance(a, b)
    return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2)
end

-- The NPCs within TAB_RANGE of the player, nearest first.
local function nearbyNpcs()
    local list, from = {}, position(player)
    for id, c in pairs(clones) do
        if c.template then
            local d = distance(position(c.entity), from)
            if d <= TAB_RANGE then list[#list + 1] = {id = id, d = d} end
        end
    end
    table.sort(list, function(a, b) return a.d < b.d end)
    return list
end

local function tabTarget()
    local list = nearbyNpcs()
    if #list == 0 then return end
    local next = 1
    for i, n in ipairs(list) do
        if n.id == target then next = i % #list + 1 end
    end
    selectTarget(list[next].id)
end

-- Distance along the ray to the sphere, or nil if it misses.
local function raySphere(origin, dir, center, radius)
    local ox, oy, oz = origin.x - center.x, origin.y - center.y, origin.z - center.z
    local b = ox * dir.x + oy * dir.y + oz * dir.z
    local c = ox * ox + oy * oy + oz * oz - radius * radius
    local disc = b * b - c
    if disc < 0 then return nil end
    local t = -b - math.sqrt(disc)
    return t >= 0 and t or nil
end

local function pick(origin, dir)
    local best, bestT
    for id, c in pairs(clones) do
        if c.template then
            local p = position(c.entity)
            local shape = PICK[c.template] or PICK_DEFAULT
            local t = raySphere(origin, dir, {x = p.x, y = p.y + shape.y, z = p.z}, shape.radius)
            if t and (not bestT or t < bestT) then best, bestT = id, t end
        end
    end
    local ground = kast.physics.raycast(origin, dir, 1000)
    if best and ground and ground.distance < bestT then return nil end -- behind a hill
    return best
end

-- A left press that ends without dragging and did not start on the GUI is a click; the ray is taken at the press,
-- before scripts/player.lua locks the cursor for a possible drag.
local function watchClick(inWorld)
    local down = kast.input.mouseDown("left")
    if pressed("mouse", down) then
        click = nil
        if inWorld and not (kast.gui and kast.gui.mouseOver()) then
            local origin, dir = kast.input.mouseRay()
            if origin then click = {origin = origin, dir = dir, travel = 0} end
        end
    elseif down and click then
        local dx, dy = kast.input.mouseDelta()
        click.travel = click.travel + math.abs(dx) + math.abs(dy)
    elseif not down and click then
        if click.travel <= CLICK_SLOP then
            local id = pick(click.origin, click.dir)
            if id then selectTarget(id) end
        end
        click = nil
    end
end

local function attack()
    if target and not dead then kast.world.send("combat.attack", {target = target}) end
end

local function playOnce(id, clip)
    local c = clones[id]
    if not c or c.template or id == me then return end -- NPCs are not animated; the local player is player.lua's
    kast.playAnimation(c.entity, clip, {loop = false, fade = 0.1})
    oneShots[c.entity] = clip ~= DEATH_CLIP or nil
end

local function forget(id)
    if id == target then clearTarget() end
    npcIds[id], clones[id] = nil, nil
end

local function forgetAll()
    clearTarget()
    npcIds, clones, oneShots = {}, {}, {}
end

return {
    init = function()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        kast.subscribe("world.entered", function(e) me, dead = e.entityId, false end)
        kast.subscribe("world.spawn", function(e)
            if e.kind == "npc" then npcIds[e.entityId] = e.template end
        end)
        kast.subscribe("world.entitySpawned", function(e)
            clones[e.entityId] = {entity = e.entity, name = e.name, template = npcIds[e.entityId]}
        end)
        kast.subscribe("world.despawn", function(e) forget(e.entityId) end)
        kast.subscribe("world.mapChanged", forgetAll)
        kast.subscribe("combat.target_next", function() if not dead and player then tabTarget() end end)
        kast.subscribe("combat.target_clear", clearTarget)
        kast.subscribe("combat.attack_target", attack)
        kast.subscribe("world.disconnected", forgetAll)

        kast.subscribe("server.combat.attack", function(e) playOnce(e.attacker, ATTACK_CLIP) end)
        kast.subscribe("server.combat.hit", function(e)
            playOnce(e.target, HIT_CLIP)
            if e.attacker == me then
                log(string.format("You hit %s for %d", e.targetName, e.damage))
            elseif e.target == me then
                log(string.format("%s hits you for %d", e.attackerName, e.damage))
            else
                log(string.format("%s hits %s for %d", e.attackerName, e.targetName, e.damage))
            end
        end)
        kast.subscribe("server.combat.death", function(e)
            playOnce(e.entity, DEATH_CLIP)
            if e.entity == target then clearTarget() end
            if e.entity == me then dead = true end
            log(e.entity == me and "You die" or e.name .. " dies")
        end)
        kast.subscribe("server.combat.revive", function(e)
            if e.entity == me then dead = false end
            log(e.entity == me and "You revive" or e.name .. " revives")
        end)
        kast.subscribe("server.combat.rejected", function(e) log("* " .. (REJECTED[e.reason] or e.reason)) end)
    end,

    update = function(dt)
        local inWorld = kast.world and kast.world.state() == "inWorld"
        if not inWorld or not player then return end
        local keys = not dead and not (kast.gui and kast.gui.focusedField())
        local function key(name) return keys and kast.input.keyDown(name) end
        if pressed("Tab", key("Tab")) then tabTarget() end
        if pressed("Escape", key("Escape")) then clearTarget() end
        if pressed("1", key("1")) then attack() end
        watchClick(inWorld)
        for entity in pairs(oneShots) do -- an attack or hit clip ended: back to standing
            local ok, state = pcall(kast.getAnimationState, entity)
            if not ok then
                oneShots[entity] = nil
            elseif not state.playing then
                kast.playAnimation(entity, IDLE_CLIP, {loop = true, fade = 0.1})
                oneShots[entity] = nil
            end
        end
    end,
}
