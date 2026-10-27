-- HUD (ui/hud.rml): the local player's Health, Wallet and Class as the world server replicates them
-- (components/health.lua, wallet.lua, class.lua: the same files the server loads), plus the character and map name; and
-- the target frame: the name, Health and portrait (PORTRAITS, by the target's kind) of the NPC scripts/combat.lua
-- selected ("target.selected" / "target.cleared"), hidden without one. The action bar is scripts/powers.lua's. Event-driven: it writes only when a world.component
-- update or a target change arrives (Health changes about once a second from regeneration, and once per hit).
local MAP_NAMES = {[10] = "Meadow", [11] = "Cavern"}
local PORTRAITS = {npc = "../textures/portraits/npc.png", player = "../textures/portraits/player.png"} -- placeholders
local me, target

local function health(data) return string.format("Health %d / %d", data.current, data.max) end

local function showMap(id) kast.gui.setText("hud_map", "Map " .. (MAP_NAMES[id] or tostring(id))) end

return {
    init = function()
        kast.gui.setVisible("target_frame", false)
        kast.subscribe("world.entered", function(e)
            me = e.entityId
            showMap(e.map)
        end)
        kast.subscribe("world.mapChanged", function(e) showMap(e.map) end)
        kast.subscribe("server.welcome", function(e) kast.gui.setText("hud_name", e.name) end, {replay = true}) -- replay: sent once on entering, maybe before this zone's scene loaded
        kast.subscribe("target.selected", function(e)
            target = e.entityId
            kast.gui.setText("target_name", e.name)
            kast.gui.setAttribute("target_portrait_image", "src", PORTRAITS[e.kind] or PORTRAITS.npc)
            kast.gui.setText("target_health", e.health and health(e.health) or "Health -")
            kast.gui.setVisible("target_frame", true)
        end)
        kast.subscribe("target.cleared", function()
            target = nil
            kast.gui.setVisible("target_frame", false)
        end)
        kast.subscribe("world.component", function(e)
            if e.entityId == target and e.name == "Health" then kast.gui.setText("target_health", health(e.data)) end
            if e.entityId ~= me then return end
            if e.name == "Health" then
                kast.gui.setText("hud_health", health(e.data))
            elseif e.name == "Wallet" then
                kast.gui.setText("hud_gold", "Gold " .. e.data.gold)
            elseif e.name == "Class" then
                kast.gui.setText("hud_class", e.data.id:sub(1, 1):upper() .. e.data.id:sub(2))
            end
        end)
    end,
    update = function(dt) end,
}
