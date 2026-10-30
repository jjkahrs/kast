-- Class powers, client side: the HUD's action bar (ui/hud.rml), keys 1-3, the power requests and their effects. The
-- server (systems/combat.lua) decides everything. On entering it sends "class.powers" {class, gcd, powers = {{id, name,
-- key, cooldown, effect}, ...}}: the bar shows exactly those, one button per key, "<key> <name>".
-- Key 1-3 (the "power1".."power3" actions; the pad's X, Y and B), or a click on a bar button (its data-event,
-- "combat.power" {id = "power_<key>"}, which tests publish too), asks kast.world.request("combat.power", {power = <id>,
-- target = <the target of scripts/combat.lua's "target.selected">}), predicted: the power's cooldown and the global cooldown start at once, and "power.predicted"
-- {power, cooldown = seconds left} goes on the bus (scripts/player.lua plays the attack clip). The server refusing it (or not answering in 2 s:
-- "timeout") rolls the cooldowns back (those this press started and nothing since replaced) and publishes
-- "power.rejected" {power, reason, predicted, cooldown} (scripts/player.lua stops the clip, scripts/combat.lua logs the
-- reason). A press while the power still cools down here predicts nothing: the server's refusal is only logged.
-- Keys typed into a GUI text field do nothing, and neither does anything while the local player is dead. A button
-- greys (class "power cooling") and counts down the seconds left while its own cooldown or the global cooldown runs,
-- and its wipe (power_<key>_wipe) darkens the part still to go: a conic-gradient mask whose edge sweeps clockwise from
-- 12 o'clock as that cooldown's fraction left runs out.
-- The server's "combat.power" with caster = me starts them too unless already running (a press the client gave up on).
-- Effects, from the server's "combat.power" for every player in view (the local one too), each a primitive like
-- scripts/combat.lua's target marker: "bolt" a small sphere flying from the caster to the target in BOLT_SECONDS;
-- "nova" a frost ring of the power's radius around the caster and "heal" a glow on it, both for FLASH_SECONDS;
-- "shield" and "evasion" a see-through sphere around the caster until its "combat.fade". The power's LOOK picks the
-- material. Powers without damage get a combat log line here ("You cast Heal for 40", "Shield Wall fades"); hits are
-- scripts/combat.lua's.
--
-- Bandwidth: one ClientEvent per key press or click (the server takes 10 a second at most); everything else is local.
local BOLT_SECONDS = 0.35
local FLASH_SECONDS = 0.6
local LOOK = { -- by power id: the material of its effect
    firebolt = "materials/fire.mat", arcane_blast = "materials/arcane.mat", smite = "materials/holy.mat",
    frost_nova = "materials/frost.mat", heal = "materials/holy.mat", shield_wall = "materials/steel.mat",
    power_shield = "materials/holy.mat", evasion = "materials/shadow.mat",
}
local NOVA_RADIUS = 8 -- m, systems/combat.lua's Frost Nova
local CHEST = 1.2     -- m above the feet: where bolts leave and land

local me, player, dead
local powers = {}     -- key -> {id, name, key, cooldown, effect}
local gcd = 0         -- seconds
local readyAt = {}    -- power id -> time its own cooldown ends
local gcdUntil = 0
local now = 0
local shown = {}      -- key -> the button text last written
local swept = {}      -- key -> the wipe angle (degrees) last written
local target          -- server id of the selected NPC
local entities = {}   -- server id -> {entity = clone, name =}
local bolts = {}      -- {entity, from = server id, to = server id, t = seconds}
local flashes = {}    -- effect entity -> seconds left
local auras = {}      -- caster server id -> {[power id] = effect entity}

local function log(text) kast.publish("chat.local", {text = text}) end

-- Seconds until power may be used again: its own cooldown or the global one, whichever ends last.
local function cooldownLeft(power) return math.max((readyAt[power.id] or 0) - now, gcdUntil - now, 0) end

-- The fraction (0-1) of that cooldown still to go: of the power's own, or of the global one if it ends last.
local function fractionLeft(power)
    local own, global = (readyAt[power.id] or 0) - now, gcdUntil - now
    local left, total = own, power.cooldown
    if global > own then left, total = global, gcd end
    return total > 0 and math.min(math.max(left / total, 0), 1) or 0
end

-- The bar: name and key per button, greyed with the seconds left and the wipe while cooling down. Writes only what
-- changed (the wipe's angle in whole degrees).
local function refreshBar()
    for key = 1, 3 do
        local power = powers[key]
        local text, cooling = "", false
        if power then
            local left = cooldownLeft(power)
            cooling = left > 0
            text = cooling and string.format("%d %s (%d)", key, power.name, math.ceil(left))
                or string.format("%d %s", key, power.name)
        end
        if shown[key] ~= text then
            shown[key] = text
            kast.gui.setText("power_" .. key .. "_label", text)
            kast.gui.setAttribute("power_" .. key, "class", cooling and "power cooling" or "power")
        end
        local angle = cooling and math.floor(360 * (1 - fractionLeft(power)) + 0.5)
        if angle and swept[key] ~= angle then
            swept[key] = angle
            kast.gui.setAttribute("power_" .. key .. "_wipe", "style", string.format(
                "mask-image: conic-gradient(#00000000 0deg, #00000000 %ddeg, #000000ff %ddeg)", angle, angle))
        end
    end
end

-- Asks for the power on key; predicted unless it is still cooling down here (the server then says why not).
local function use(key)
    local power = powers[key]
    if not power or dead then return end
    local predicted = cooldownLeft(power) <= 0
    local before, mine = {ready = readyAt[power.id], gcd = gcdUntil}, {}
    kast.world.request("combat.power", {power = power.id, target = target}, {
        predict = predicted and function()
            mine.ready, mine.gcd = now + power.cooldown, now + gcd
            readyAt[power.id], gcdUntil = mine.ready, mine.gcd
            refreshBar()
            kast.publish("power.predicted", {power = power.id, cooldown = cooldownLeft(power)})
        end or nil,
        rejected = function(reason)
            if predicted then -- what this press started, unless something since replaced it
                if readyAt[power.id] == mine.ready then readyAt[power.id] = before.ready end
                if gcdUntil == mine.gcd then gcdUntil = before.gcd end
                refreshBar()
            end
            kast.publish("power.rejected", {power = power.id, reason = reason, predicted = predicted,
                                            cooldown = cooldownLeft(power)})
        end,
    })
end

local function entityOf(id) return id == me and player or (entities[id] and entities[id].entity) end

local function nameOf(id) return entities[id] and entities[id].name or tostring(id) end

local function position(id)
    local entity = entityOf(id)
    local ok, tf = pcall(kast.get, entity, "Transform") -- its clone may be gone
    return ok and tf.position or nil
end

-- A primitive under parent (nil: the scene root), at the local position, scale and material given.
local function effect(name, parent, pos, scale, material)
    local e = kast.create(name, parent)
    kast.set(e, "Transform", {position = pos, scale = scale})
    kast.add(e, "MeshRenderer")
    kast.set(e, "MeshRenderer", {primitive = "sphere", material = material, castShadows = false})
    return e
end

local function removeAura(caster, power)
    local list = auras[caster]
    if not list or not list[power] then return end
    pcall(kast.destroy, list[power]) -- gone already if its parent clone was
    list[power] = nil
end

local function clearEffects()
    for _, b in ipairs(bolts) do pcall(kast.destroy, b.entity) end
    for e in pairs(flashes) do pcall(kast.destroy, e) end
    for caster, list in pairs(auras) do
        for power in pairs(list) do removeAura(caster, power) end
    end
    bolts, flashes, auras = {}, {}, {}
end

local function show(e)
    local caster = entityOf(e.caster)
    if not caster then return end
    local material = LOOK[e.power] or "materials/gold.mat"
    if e.effect == "bolt" then
        local from = position(e.caster)
        if not from then return end
        local bolt = effect("Bolt", nil, {x = from.x, y = from.y + CHEST, z = from.z}, {x = 0.35, y = 0.35, z = 0.35}, material)
        bolts[#bolts + 1] = {entity = bolt, from = e.caster, to = e.target, t = 0}
    elseif e.effect == "nova" then
        local d = NOVA_RADIUS * 2
        flashes[effect("Frost Nova", caster, {x = 0, y = 0.05, z = 0}, {x = d, y = 0.1, z = d}, material)] = FLASH_SECONDS
    elseif e.effect == "heal" then
        flashes[effect("Heal", caster, {x = 0, y = 1, z = 0}, {x = 1.6, y = 2.2, z = 1.6}, material)] = FLASH_SECONDS
    elseif e.effect == "shield" or e.effect == "evasion" then
        removeAura(e.caster, e.power)
        auras[e.caster] = auras[e.caster] or {}
        auras[e.caster][e.power] = effect(e.name, caster, {x = 0, y = 1, z = 0}, {x = 1.5, y = 2.3, z = 1.5}, material)
    end
end

-- Log lines for the powers that deal no damage (scripts/combat.lua logs hits).
local function logPower(e)
    local who = e.caster == me and "You cast " or e.casterName .. " casts "
    if e.effect == "nova" then
        local rooted = {}
        for _, id in ipairs(e.rooted or {}) do rooted[#rooted + 1] = nameOf(id) end
        log(who .. e.name .. (#rooted > 0 and ": " .. table.concat(rooted, ", ") .. " rooted" or ""))
    elseif e.effect == "heal" then
        log(string.format("%s%s for %d", who, e.name, e.amount))
    elseif e.effect == "shield" or e.effect == "evasion" then
        log(who .. e.name)
    end
end

local function moveBolts(dt)
    for i = #bolts, 1, -1 do
        local b = bolts[i]
        b.t = b.t + dt
        local from, to = position(b.from), position(b.to)
        if not from or not to or b.t >= BOLT_SECONDS then
            pcall(kast.destroy, b.entity)
            table.remove(bolts, i)
        else
            local k = b.t / BOLT_SECONDS
            kast.set(b.entity, "Transform", {position = {x = from.x + (to.x - from.x) * k,
                y = from.y + CHEST + (to.y - from.y) * k, z = from.z + (to.z - from.z) * k}})
        end
    end
end

return {
    init = function()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        kast.gui.setVisible("action_bar", false)
        kast.subscribe("world.entered", function(e) me, dead = e.entityId, false end)
        kast.subscribe("world.entitySpawned", function(e) entities[e.entityId] = {entity = e.entity, name = e.name} end)
        kast.subscribe("world.despawn", function(e)
            for power in pairs(auras[e.entityId] or {}) do removeAura(e.entityId, power) end
            entities[e.entityId] = nil
        end)
        kast.subscribe("world.mapChanged", function()
            clearEffects()
            entities = {}
        end)
        kast.subscribe("world.disconnected", clearEffects)
        kast.subscribe("target.selected", function(e) target = e.entityId end)
        kast.subscribe("target.cleared", function() target = nil end)
        kast.subscribe("combat.power", function(e)
            local key = tonumber(e.id and e.id:match("^power_(%d)$"))
            if key then use(key) end
        end)
        kast.subscribe("server.class.powers", function(e)
            powers, gcd, readyAt, gcdUntil, shown, swept = {}, e.gcd, {}, 0, {}, {}
            for _, power in ipairs(e.powers) do powers[power.key] = power end
            kast.gui.setVisible("action_bar", true)
            refreshBar()
        end, {replay = true}) -- replay: sent once on entering, maybe before this zone's scene loaded
        kast.subscribe("server.combat.power", function(e)
            if e.caster == me then -- predicted already, unless use() gave up on it (timeout)
                for _, power in pairs(powers) do
                    if power.id == e.power and (readyAt[power.id] or 0) <= now then
                        readyAt[power.id] = now + power.cooldown
                    end
                end
                if gcdUntil <= now then gcdUntil = now + gcd end
            end
            show(e)
            logPower(e)
        end)
        kast.subscribe("server.combat.fade", function(e)
            removeAura(e.entity, e.power)
            log(e.entity == me and e.name .. " fades" or nameOf(e.entity) .. "'s " .. e.name .. " fades")
        end)
        kast.subscribe("server.combat.death", function(e) if e.entity == me then dead = true end end)
        kast.subscribe("server.combat.revive", function(e) if e.entity == me then dead = false end end)
    end,

    update = function(dt)
        now = now + dt
        moveBolts(dt)
        for e, left in pairs(flashes) do
            if left <= dt then
                pcall(kast.destroy, e)
                flashes[e] = nil
            else
                flashes[e] = left - dt
            end
        end
        if not (kast.world and kast.world.state() == "inWorld") then return end
        refreshBar()
        local keys = not dead and not (kast.gui and kast.gui.focusedField())
        for key = 1, 3 do
            if keys and kast.input.actionPressed("power" .. key) then use(key) end
        end
    end,
}
