-- Nameplates (ui/nameplate.rml: the Overlay WorldUI on prefabs/player.prefab, remote_player.prefab and the NPC
-- templates in npc_templates.prefab): each character's name over its head, and an HP bar from its replicated Health
-- (components/health.lua) once the server sends one. Friendly NPCs (no Health, see scripts/combat.lua) show the name
-- only. The dead (Health 0: corpses, a dead player) have theirs hidden (WorldUI visible off) until they revive: a corpse
-- lies tipped on its side (scripts/combat.lua), which would swing its anchor down beside it. The local player's name is
-- the server's "server.welcome" (none offline).
-- Damage numbers: when a character's replicated Health.current drops, kast.gui.worldText shows the loss just under its
-- nameplate (class "damage", the engine's default red). Rises (regeneration, heals, revives) show nothing: regeneration
-- would show one every second.
-- Event-driven: it writes only when world.entitySpawned, world.component or server.welcome arrive.
--
-- Bandwidth: none; everything here is local.
local TEXT_LIFETIME = 1 -- s, the default world-text animation's length
local TEXT_BELOW = 0.3 -- m under the nameplate's anchor

local player -- the local player's scene entity
local me     -- its server entity id
local plates = {} -- server id -> {entity = scene entity, current = last Health.current}

local function plateOf(id)
    if id == me and player then
        plates[id] = plates[id] or {entity = player}
    end
    return plates[id]
end

local function showDamage(entity, amount)
    local p = kast.get(entity, "Transform").position
    local offset = kast.get(entity, "WorldUI").offset
    kast.gui.worldText({x = p.x + offset.x, y = p.y + offset.y - TEXT_BELOW, z = p.z + offset.z},
                       string.format("%d", math.floor(amount + 0.5)), "damage", TEXT_LIFETIME)
end

local function showHealth(plate, health)
    local e = plate.entity
    local dead = health.current <= 0
    local percent = math.floor(100 * math.max(0, math.min(1, health.current / health.max)) + 0.5)
    kast.gui.setVisible(e, "hp", true)
    kast.gui.setAttribute(e, "hp_fill", "style", string.format("width: %d%%", percent))
    kast.set(e, "WorldUI", {visible = not dead})
    if plate.current and health.current < plate.current then showDamage(e, plate.current - health.current) end
    plate.current = health.current
end

return {
    init = function()
        for e in kast.query("NetworkLocalPlayer", "WorldUI") do player = e end
        kast.subscribe("server.welcome", function(e)
            if player then kast.gui.setText(player, "name", e.name) end
        end, {replay = true}) -- sent once on entering, maybe before this zone's scene loaded
        kast.subscribe("world.entered", function(e) me = e.entityId end)
        kast.subscribe("world.entitySpawned", function(e) -- also each clone a zone switch re-creates
            if not kast.has(e.entity, "WorldUI") then return end
            plates[e.entityId] = {entity = e.entity}
            kast.gui.setText(e.entity, "name", e.name)
            -- Its first Health may have come before this event (replication publishes it while handling the spawn): the
            -- clone has it already.
            if kast.has(e.entity, "Health") then showHealth(plates[e.entityId], kast.get(e.entity, "Health")) end
        end)
        kast.subscribe("world.component", function(e)
            local plate = e.name == "Health" and plateOf(e.entityId)
            if plate then showHealth(plate, e.data) end
        end)
        kast.subscribe("world.despawn", function(e) plates[e.entityId] = nil end)
        kast.subscribe("world.mapChanged", function() plates = {} end)
        kast.subscribe("world.disconnected", function() plates = {} end)
    end,
    update = function(dt) end,
}
