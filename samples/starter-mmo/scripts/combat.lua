-- Tab-target combat, client side. The server (systems/combat.lua) decides every hit; this only picks a target and shows
-- what the server sends back (scripts/powers.lua asks for the powers).
-- Targeting: Tab cycles the NPCs in view, nearest first, within TAB_RANGE; a left click (press and release without
-- dragging, not on the GUI) on an NPC selects it (kast.input.mouseRay against each NPC's PICK sphere, unless the
-- terrain is closer); Escape clears. The target clears when its NPC despawns or dies (or the map changes). A gold disc
-- (MARKER_*) under the target shows it. "target.selected" {entityId, name, health} / "target.cleared" go on the bus for the
-- HUD's target frame (scripts/hud.lua).
-- Dead NPCs (replicated Health 0) are corpses: never targeted, tipped onto their side. A right click (press and release
-- without dragging, not on the GUI) on a corpse publishes "loot.clicked" {entityId} for scripts/loot.lua.
-- Keys typed into a GUI text field do nothing, as in scripts/player.lua, and neither do they while the local player is
-- dead. The keys' actions are also bus events, "combat.target_next" and "combat.target_clear", for buttons (and tests)
-- to publish.
-- The server's combat.* events become combat log lines in the chat box ("chat.local", scripts/chat.lua): hits (with
-- the power's name, and what a shield absorbed), dodges, deaths, revives and refusals ("* ..."); other players' clones
-- play the attack (on any power they use), hit and death clips (the local player's are scripts/player.lua's).
-- Each hit throws a one-shot burst of sparks (SPARK_*, textures/particles/spark.png) at the target's chest: an entity
-- with a stopped, non-looping ParticleEmitter and kast.particles.burst, destroyed once kast.particles.alive reaches 0.
--
-- Bandwidth: none; everything here is local.
local TAB_RANGE = 40 -- m
local CLICK_SLOP = 4 -- pixels of mouse travel a click may have; more is a camera drag
local PICK = {Guard = {y = 1.1, radius = 1.2}, Bat = {y = 2.25, radius = 1.0}} -- by template: sphere over the feet
local PICK_DEFAULT = {y = 1, radius = 1}
local MARKER_SCALE = {x = 2, y = 0.06, z = 2}
local MARKER_MATERIAL = "materials/gold.mat"
local SPARK_COUNT, SPARK_HEIGHT = 24, 1.2 -- particles, m above the target's feet
local SPARK_EMITTER = {
    playOnStart = false, looping = false, rate = 0, maxParticles = SPARK_COUNT, shape = "sphere", radius = 0.1,
    lifetimeMin = 0.25, lifetimeMax = 0.5, speedMin = 3, speedMax = 6, startSizeMin = 0.25, startSizeMax = 0.4,
    endSize = 0.3, startColor = {x = 6, y = 4, z = 1.2}, endColor = {x = 3, y = 0.6, z = 0.1}, startAlpha = 1,
    endAlpha = 0, gravity = {x = 0, y = -9.8, z = 0}, texture = "textures/particles/spark.png", blend = "additive",
}
local ATTACK_CLIP, HIT_CLIP, DEATH_CLIP, IDLE_CLIP = "Punch_Jab", "Hit_Chest", "Death_A", "Idle_A"
local REJECTED = {
    range = "Out of range",
    cooldown = "Not ready yet",
    gcd = "Not ready yet (global cooldown)",
    target = "Invalid target",
    target_dead = "Your target is dead",
    not_behind = "Not behind target",
    unknown_power = "Unknown power",
    wrong_class = "Your class can't use that",
    dead = "You are dead",
    timeout = "No answer from the server",
    disconnected = "Disconnected",
}

local me                -- the local player's server entity id
local dead = false      -- the local player is dead: no targeting keys
local player            -- the local player's scene entity
local clones = {}       -- server id -> {entity = clone, name =, template = (NPCs)}
local target, marker    -- the target's server id, the marker entity under its clone
local held = {}         -- key or button name -> down last frame
local clicks = {}       -- button -> the press being watched: {origin, dir, travel}
local oneShots = {}     -- remote player clone -> true while an attack or hit clip plays
local sparks = {}       -- spark burst entities still flying

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

local function isDead(c)
    return kast.has(c.entity, "Health") and kast.get(c.entity, "Health").current <= 0
end

-- Lays a corpse's clone on its side: its heading turn, then a quarter roll about its own forward axis.
local function tip(c, dead)
    if c.tipped or not (dead or isDead(c)) then return end
    c.tipped = true
    local q = kast.get(c.entity, "Transform").rotation -- a yaw: {0, sin(h/2), 0, cos(h/2)}
    local r = math.sqrt(0.5)
    kast.set(c.entity, "Transform", {rotation = {x = q.y * r, y = q.y * r, z = q.w * r, w = q.w * r}})
end

local function distance(a, b)
    return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2)
end

-- The NPCs within TAB_RANGE of the player, nearest first.
local function nearbyNpcs()
    local list, from = {}, position(player)
    for id, c in pairs(clones) do
        if c.template and not isDead(c) then
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

-- The NPC (corpses: dead ones only, else living ones) the ray hits first, unless the terrain is closer.
local function pick(origin, dir, corpses)
    local best, bestT
    for id, c in pairs(clones) do
        if c.template and isDead(c) == corpses then
            local p = position(c.entity)
            local shape = PICK[c.template] or PICK_DEFAULT
            local center = {x = p.x, y = p.y + shape.y, z = p.z}
            local radius = shape.radius
            if corpses then center, radius = {x = p.x, y = p.y + 0.5, z = p.z}, shape.y + shape.radius end -- lying down
            local t = raySphere(origin, dir, center, radius)
            if t and (not bestT or t < bestT) then best, bestT = id, t end
        end
    end
    local ground = kast.physics.raycast(origin, dir, 1000)
    if best and ground and ground.distance < bestT then return nil end -- behind a hill
    return best
end

-- A press of button that ends without dragging and did not start on the GUI is a click; the ray is taken at the press,
-- before scripts/player.lua locks the cursor for a possible drag. Returns the click's ray on its release frame.
local function watchClick(inWorld, button)
    local down = kast.input.mouseDown(button)
    local click = clicks[button]
    if pressed(button, down) then
        clicks[button] = nil
        if inWorld and not (kast.gui and kast.gui.mouseOver()) then
            local origin, dir = kast.input.mouseRay()
            if origin then clicks[button] = {origin = origin, dir = dir, travel = 0} end
        end
    elseif down and click then
        local dx, dy = kast.input.mouseDelta()
        click.travel = click.travel + math.abs(dx) + math.abs(dy)
    elseif not down and click then
        clicks[button] = nil
        if click.travel <= CLICK_SLOP then return click end
    end
end

local function playOnce(id, clip)
    local c = clones[id]
    if not c or c.template or id == me then return end -- NPCs are not animated; the local player is player.lua's
    kast.playAnimation(c.entity, clip, {loop = false, fade = 0.1})
    oneShots[c.entity] = clip ~= DEATH_CLIP or nil
end

local function sparkAt(id)
    local c = clones[id]
    local entity = c and c.entity or (id == me and player)
    if not entity then return end
    local p = position(entity)
    local spark = kast.create("Hit Sparks")
    kast.set(spark, "Transform", {position = {x = p.x, y = p.y + SPARK_HEIGHT, z = p.z}})
    kast.add(spark, "ParticleEmitter")
    kast.set(spark, "ParticleEmitter", SPARK_EMITTER)
    kast.particles.burst(spark, SPARK_COUNT)
    sparks[spark] = true
end

local function forget(id)
    if id == target then clearTarget() end
    clones[id] = nil
end

local function forgetAll()
    clearTarget()
    clones, oneShots = {}, {}
end

return {
    init = function()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        kast.subscribe("world.entered", function(e) me, dead = e.entityId, false end)
        kast.subscribe("world.entitySpawned", function(e) -- also each clone a zone switch re-creates
            clones[e.entityId] = {entity = e.entity, name = e.name, template = e.kind == "npc" and e.template or nil}
            if e.kind == "npc" then tip(clones[e.entityId]) end -- a corpse coming into view
        end)
        kast.subscribe("world.component", function(e)
            local c = clones[e.entityId]
            if c and c.template and e.name == "Health" then tip(c, e.data.current <= 0) end
        end)
        kast.subscribe("world.despawn", function(e) forget(e.entityId) end)
        kast.subscribe("world.mapChanged", forgetAll)
        kast.subscribe("combat.target_next", function() if not dead and player then tabTarget() end end)
        kast.subscribe("combat.target_clear", clearTarget)
        kast.subscribe("world.disconnected", forgetAll)

        kast.subscribe("server.combat.power", function(e) playOnce(e.caster, ATTACK_CLIP) end)
        kast.subscribe("server.combat.hit", function(e)
            playOnce(e.target, HIT_CLIP)
            sparkAt(e.target)
            if e.power then -- a player's power
                if e.attacker == me then
                    log(string.format("You cast %s on %s for %d", e.power, e.targetName, e.damage))
                else
                    log(string.format("%s casts %s on %s for %d", e.attackerName, e.power, e.targetName, e.damage))
                end
            elseif e.target == me then
                log(string.format("%s hits you for %d", e.attackerName, e.damage))
            else
                log(string.format("%s hits %s for %d", e.attackerName, e.targetName, e.damage))
            end
            if e.absorbed and e.absorbed > 0 then log(string.format("%s absorbs %d", e.absorbedBy, e.absorbed)) end
        end)
        kast.subscribe("server.combat.dodge", function(e)
            if e.target == me then
                log(string.format("You dodge %s's attack", e.attackerName))
            else
                log(string.format("%s dodges %s's attack", e.targetName, e.attackerName))
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
        kast.subscribe("power.rejected", function(e) log("* " .. (REJECTED[e.reason] or e.reason)) end) -- predicted
    end,

    update = function(dt)
        for spark in pairs(sparks) do -- burnt out (or gone with the scene): destroyed
            local ok, alive = pcall(kast.particles.alive, spark)
            if not ok or alive == 0 then
                pcall(kast.destroy, spark)
                sparks[spark] = nil
            end
        end
        local inWorld = kast.world and kast.world.state() == "inWorld"
        if not inWorld or not player then return end
        local keys = not dead and not (kast.gui and kast.gui.focusedField())
        local function key(name) return keys and kast.input.keyDown(name) end
        if pressed("Tab", key("Tab")) then tabTarget() end
        if pressed("Escape", key("Escape")) then clearTarget() end
        local left = watchClick(inWorld, "left")
        if left then
            local id = pick(left.origin, left.dir, false)
            if id then selectTarget(id) end
        end
        local right = watchClick(inWorld, "right")
        if right then
            local id = pick(right.origin, right.dir, true)
            if id then kast.publish("loot.clicked", {entityId = id}) end
        end
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
