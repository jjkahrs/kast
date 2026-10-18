-- Chat box (ui/chat.rml): Send -> kast.world.say; every world.chat line (the sender's own too), the server's
-- welcome / player.arrived events (systems/events.lua) and other scripts' "chat.local" {text} lines (the combat log,
-- scripts/combat.lua) go into the log, which keeps the last LINES lines.
local LINES = 8
local lines = {}

local function add(line)
    lines[#lines + 1] = line
    if #lines > LINES then table.remove(lines, 1) end
    kast.gui.setText("chat_log", table.concat(lines, "\n"))
end

return {
    init = function()
        kast.subscribe("chat.send", function()
            local text = kast.gui.value("chat_input")
            if text == "" or not kast.world or kast.world.state() ~= "inWorld" then return end
            kast.gui.setText("chat_input", "")
            kast.world.say(text)
        end)
        kast.subscribe("world.chat", function(e) add(e.from .. ": " .. e.text) end)
        kast.subscribe("server.welcome", function(e) add("* Welcome to the Meadow, " .. e.name .. ".") end)
        kast.subscribe("server.player.arrived", function(e) add("* " .. e.name .. " is here.") end)
        kast.subscribe("chat.local", function(e) add(e.text) end)
    end,
    update = function(dt) end,
}
