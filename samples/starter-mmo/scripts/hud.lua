-- HUD (ui/hud.rml): the local player's Health and Wallet as the world server replicates them (components/health.lua,
-- components/wallet.lua: the same files the server loads), plus the character and map name. Event-driven: it writes
-- only when a world.component update arrives, at most about once a second (the server's systems/regen.lua).
local MAP_NAMES = {[10] = "Meadow", [11] = "Cavern"}
local me

local function showMap(id) kast.gui.setText("hud_map", "Map " .. (MAP_NAMES[id] or tostring(id))) end

return {
    init = function()
        kast.subscribe("world.entered", function(e)
            me = e.entityId
            showMap(e.map)
        end)
        kast.subscribe("world.mapChanged", function(e) showMap(e.map) end)
        kast.subscribe("server.welcome", function(e) kast.gui.setText("hud_name", e.name) end)
        kast.subscribe("world.component", function(e)
            if e.entityId ~= me then return end
            if e.name == "Health" then
                kast.gui.setText("hud_health", string.format("Health %d / %d", e.data.current, e.data.max))
            elseif e.name == "Wallet" then
                kast.gui.setText("hud_gold", "Gold " .. e.data.gold)
            end
        end)
    end,
    update = function(dt) end,
}
