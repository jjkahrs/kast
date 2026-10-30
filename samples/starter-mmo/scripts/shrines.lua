-- Interactable zone objects (the Meadow Shrine, maps/meadow/objects): the "use" action (E, or the pad's D-pad up;
-- project.kast "input") activates the nearest one within REACH m of the character (kast.nearestInteractable,
-- kast.interact; the engine binds no keys, this script does). Walking into
-- reach tells the player once per approach. The server checks the range itself; its Lua (systems/shrines.lua) answers
-- with "shrine.blessing", shown in the chat box (scripts/chat.lua, "chat.local").
local REACH = 3 -- m from the object's edge: the shrine's server range
local KEY = "E"
local player, inReach = nil, nil

local function chat(text) kast.publish("chat.local", {text = text}) end

return {
    init = function()
        kast.subscribe("server.shrine.blessing", function(e) chat("* " .. e.text) end)
        kast.subscribe("world.entered", function() player, inReach = nil, nil end)
    end,
    update = function(dt)
        if not (kast.world and kast.world.state() == "inWorld") then
            return
        end
        if not player then
            for e in kast.query("NetworkLocalPlayer") do player = e end
            if not player then return end
        end
        local near = kast.nearestInteractable(kast.get(player, "Transform").position, REACH)
        if near and near.id ~= inReach then chat(string.format("[%s] Use the %s", KEY, near.name ~= "" and near.name or "object")) end
        inReach = near and near.id
        if kast.input.actionPressed("use") and not (kast.gui and kast.gui.focusedField()) then
            if near then kast.interact(near.id) else chat("There is nothing here to use.") end
        end
    end,
}
