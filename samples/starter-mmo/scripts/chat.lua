-- Chat box (ui/chat.rml). Enter focuses the input field, Enter again (or Send) sends what it holds: plain text is
-- say (kast.world.say), and slash commands as in WoW:
--   /s text          say, the players in view               /y text      yell, the whole map within its range
--   /me text, /e     emote: "Name text"                      /z text      the zone channel (everyone on this map)
--   /w name text     whisper, any map                        /1 /2 /3 text  the channel with that number
--   /join name       join General, Trade or LFG              /leave name  leave one
--   /ignore name     hide name's whispers and channel lines  /unignore name
--   /mute name min   GM: mute name for min minutes           /unmute name
-- The log keeps the last LINES lines, each coloured by kind: say/yell/emote (world.chat), whispers, channel and zone
-- lines from the chat service (kast.chat, chat.message), its notices and errors, other scripts' "chat.local" {text}
-- lines (the combat log, scripts/combat.lua) and the server's welcome / player.arrived events (systems/events.lua).
-- A system line says when the chat service is unavailable and when it is back.
local LINES = 10
local lines = {}
local enterWasDown = false
local unavailable = false -- the last chat.status said so

local function add(text, kind)
    lines[#lines + 1] = {text = text, kind = kind or "say"}
    if #lines > LINES then table.remove(lines, 1) end
    for i = 1, LINES do
        local line = lines[i]
        kast.gui.setText("chat_line_" .. i, line and line.text or "")
        kast.gui.setAttribute("chat_line_" .. i, "class", "chat_line chat_" .. (line and line.kind or "say"))
    end
end

local function system(text) add(text, "system") end

local ERRORS = {
    not_online = function(e) return string.format("No player named '%s' is currently playing.", e.name) end,
    muted = function(e) return string.format("You are muted (%d more minute%s).", e.minutes, e.minutes == 1 and "" or "s") end,
    not_allowed = function() return "You are not allowed to do that." end,
    no_channel = function(e) return string.format("No such channel: %s.", e.name) end,
    not_in_channel = function(e) return string.format("You are not in channel %s.", e.name) end,
    rate_limited = function() return "You are sending messages too fast." end,
    rejected = function() return "Message not sent." end,
    bad_name = function(e) return string.format("'%s' is not a character name.", e.name) end,
}
local NOTICES = {
    ignored = function(e) return string.format("%s is now being ignored.", e.name) end,
    unignored = function(e) return string.format("%s is no longer being ignored.", e.name) end,
    mute_set = function(e) return string.format("%s is muted for %d minute%s.", e.name, e.minutes, e.minutes == 1 and "" or "s") end,
    mute_cleared = function(e) return string.format("%s is no longer muted.", e.name) end,
    you_muted = function(e) return string.format("You have been muted for %d minute%s.", e.minutes, e.minutes == 1 and "" or "s") end,
    you_unmuted = function() return "You are no longer muted." end,
}

local function inWorld() return kast.world and kast.world.state() == "inWorld" end
local function chatUp() return kast.chat and kast.chat.state() == "connected" end

-- Runs fn (a kast.chat call) if the chat service is connected; a system line otherwise.
local function viaChat(fn, ...)
    if not chatUp() then return system("The chat service is unavailable.") end
    fn(...)
end

local COMMANDS -- below: name -> function(rest)

local function say(kind, text)
    if text == "" then return end
    kast.world.say(text, kind)
end

local function send(input)
    if input:sub(1, 1) ~= "/" then return say("say", input) end
    local command, rest = input:match("^/(%S+)%s*(.*)$")
    if not command then return end
    command = command:lower()
    local number = tonumber(command)
    if number then
        if rest == "" then return end
        for _, ch in ipairs(kast.chat and kast.chat.channels() or {}) do
            if ch.number == number then return viaChat(kast.chat.send, number, rest) end
        end
        return system(chatUp() and string.format("You are not in channel %d.", number) or "The chat service is unavailable.")
    end
    local fn = COMMANDS[command]
    if not fn then return system(string.format("Unknown command /%s.", command)) end
    fn(rest)
end

local function nameAndText(rest) return rest:match("^(%S+)%s+(.+)$") end

COMMANDS = {
    s = function(rest) say("say", rest) end,
    y = function(rest) say("yell", rest) end,
    me = function(rest) say("emote", rest) end,
    w = function(rest)
        local name, text = nameAndText(rest)
        if not name then return system("Usage: /w name message") end
        viaChat(kast.chat.whisper, name, text)
    end,
    z = function(rest) if rest ~= "" then viaChat(kast.chat.zone, rest) end end,
    join = function(rest) if rest ~= "" then viaChat(kast.chat.join, rest) end end,
    leave = function(rest) if rest ~= "" then viaChat(kast.chat.leave, rest) end end,
    ignore = function(rest) if rest ~= "" then viaChat(kast.chat.ignore, rest) end end,
    unignore = function(rest) if rest ~= "" then viaChat(kast.chat.unignore, rest) end end,
    mute = function(rest)
        local name, minutes = rest:match("^(%S+)%s+(%d+)$")
        if not name or tonumber(minutes) < 1 then return system("Usage: /mute name minutes") end
        viaChat(kast.chat.mute, name, tonumber(minutes))
    end,
    unmute = function(rest) if rest ~= "" then viaChat(kast.chat.unmute, rest) end end,
}
COMMANDS.say, COMMANDS.yell, COMMANDS.e, COMMANDS.emote = COMMANDS.s, COMMANDS.y, COMMANDS.me, COMMANDS.me
COMMANDS.whisper, COMMANDS.t, COMMANDS.tell, COMMANDS.zone = COMMANDS.w, COMMANDS.w, COMMANDS.w, COMMANDS.z

local function submit()
    local text = kast.gui.value("chat_input")
    if text == "" or not inWorld() then return end
    kast.gui.setText("chat_input", "")
    send(text)
end

return {
    init = function()
        kast.subscribe("chat.send", submit)
        kast.subscribe("world.chat", function(e)
            if e.kind == "yell" then add(e.from .. " yells: " .. e.text, "yell")
            elseif e.kind == "emote" then add(e.from .. " " .. e.text, "emote")
            else add(e.from .. ": " .. e.text, "say") end
        end)
        kast.subscribe("chat.message", function(e)
            if e.kind == "whisper" then add(e.from .. " whispers: " .. e.text, "whisper")
            elseif e.kind == "whisper_sent" then add("To " .. e.to .. ": " .. e.text, "whisper")
            elseif e.kind == "channel" then add(string.format("[%s] %s: %s", e.channel, e.from, e.text), "channel")
            else add(string.format("[Zone] %s: %s", e.from, e.text), "zone") end
        end)
        kast.subscribe("chat.channel", function(e)
            system(e.joined and string.format("Joined channel: [%d. %s]", e.number, e.channel) or ("Left channel: " .. e.channel))
        end)
        kast.subscribe("chat.error", function(e)
            local text = ERRORS[e.code]
            add(text and text(e) or ("Chat error: " .. e.code), "error")
        end)
        kast.subscribe("chat.notice", function(e)
            local text = NOTICES[e.code]
            system(text and text(e) or e.code)
        end)
        kast.subscribe("chat.status", function(e)
            if not e.connected and not unavailable then system("The chat service is unavailable. Reconnecting...")
            elseif e.connected and unavailable then system("Reconnected to the chat service.") end
            unavailable = not e.connected
        end)
        kast.subscribe("server.welcome", function(e) system("* Welcome to the Meadow, " .. e.name .. ".") end)
        kast.subscribe("server.player.arrived", function(e) system("* " .. e.name .. " is here.") end)
        kast.subscribe("chat.local", function(e) add(e.text, "say") end)
        -- A scene loaded after the chat service went away (a portal) still says so.
        if inWorld() and kast.chat and kast.chat.state() == "disconnected" then
            unavailable = true
            system("The chat service is unavailable. Reconnecting...")
        end
    end,
    update = function(dt)
        -- Enter: into the input field; Enter in it sends and gives the focus back to the game (the Send button).
        local enterDown = kast.input.keyDown("Return")
        if enterDown and not enterWasDown and inWorld() then
            if kast.gui.focusedField() == "chat_input" then
                submit()
                kast.gui.focus("chat_send")
            else
                kast.gui.focus("chat_input")
            end
        end
        enterWasDown = enterDown
    end,
}
