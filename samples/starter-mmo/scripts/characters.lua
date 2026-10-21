-- Character select (scenes/characters.scene, ui/characters.rml): lists the account's characters on the realm (the
-- first four, "Play <Name> (<Class>)"), creates one, enters the world with the one clicked; scripts/world_flow.lua then
-- loads the scene of the map it entered on, whose HUD and chat show from the start. The realm screen connected; the
-- connection survives kast.loadScene.
-- The click shows the loading screen (ui/loading.rml) at once and puts the "loading" game machine (fsms/loading.lua) in
-- "entering", so the zone's scripts/loading.lua keeps it up until the zone renders. A character error, or no
-- world.entered within ENTER_TIMEOUT, hides it again and says why.
-- A new character needs a class: the four class buttons ("characters.class" {id = "class_<id>"}) pick one, highlight it
-- and show its description; Create sends kast.world.createCharacter(name, {class = <id>}), which the server's
-- systems/classes.lua checks. Without a class picked, Create only says so.
local MAX = 4
local CLASSES = { -- id -> what the class buttons show; the server decides what each class is
    warrior = {name = "Warrior", description = "A tough melee fighter who can hide behind a shield wall."},
    mage = {name = "Mage", description = "A fragile ranged caster who freezes foes in place."},
    priest = {name = "Priest", description = "A ranged holy caster who heals and shields themself."},
    rogue = {name = "Rogue", description = "A quick melee fighter who strikes from behind and dodges."},
}
local DEFAULT_CLASS = "warrior" -- what systems/classes.lua makes a character created before classes
local ENTER_TIMEOUT = 15 -- s from the click to world.entered
local ERRORS = {
    name_taken = "That name is taken on this realm.",
    name_invalid = "Names are 3-16 letters (A-Z), no spaces or digits.",
    char_limit = "No more characters on this account.",
    rate_limited = "One request at a time, please.",
    service_unavailable = "The world server can't reach its database. Try again soon.",
    options_invalid = "Pick one of the four classes.",
}
local characters = {}
local picked -- the class id picked for the next character
local entering -- s since the click, while the loading screen is up

local function status(text) kast.gui.setText("characters_status", text) end

local function stopEntering(text)
    entering = nil
    kast.gui.setVisible("loading", false)
    kast.fsm.change("loading", "idle")
    kast.log("loading: hidden, " .. text)
    status(text)
end

return {
    init = function()
        kast.gui.setVisible("loading", false)
        if kast.fsm.state("loading") ~= "idle" then kast.fsm.change("loading", "idle") end -- left by a disconnect
        for i = 1, MAX do kast.gui.setVisible("char_" .. i, false) end
        if not kast.world or kast.world.state() ~= "lobby" then
            return status("Not connected: Play scenes/login.scene to log in and pick a realm.")
        end
        kast.subscribe("world.charList", function(e)
            characters = e.characters
            for i = 1, MAX do
                local character = characters[i]
                kast.gui.setVisible("char_" .. i, character ~= nil)
                if character then
                    local class = CLASSES[character.options.class or DEFAULT_CLASS]
                    kast.gui.setText("char_" .. i, string.format("Play %s (%s)", character.name, class and class.name or "?"))
                end
            end
            status(#characters == 0 and "Create your first character below." or "")
        end)
        kast.subscribe("characters.pick", function(e)
            local character = characters[tonumber(e.id:match("^char_(%d+)$"))]
            if not character or entering then return end
            kast.gui.setText("loading_text", "Entering the world as " .. character.name .. "...")
            kast.gui.setVisible("loading", true)
            kast.fsm.change("loading", "entering")
            entering = 0
            kast.log("loading: shown for " .. character.name)
            kast.world.enterWorld(character.id)
        end)
        kast.subscribe("characters.class", function(e)
            local id = e.id:match("^class_(%a+)$")
            if not CLASSES[id] then return end
            picked = id
            for other in pairs(CLASSES) do
                kast.gui.setAttribute("class_" .. other, "class", other == id and "class_button selected" or "class_button")
            end
            kast.gui.setText("class_description", CLASSES[id].description)
        end)
        kast.subscribe("characters.create", function()
            if not picked then return status("Pick a class first.") end
            status("Creating...")
            kast.world.createCharacter(kast.gui.value("char_name"), {class = picked})
        end)
        kast.subscribe("world.charCreated", function()
            kast.gui.setText("char_name", "")
            kast.world.listCharacters()
        end)
        kast.subscribe("world.charError", function(e)
            local text = ERRORS[e.code] or e.code
            if entering then return stopEntering(text) end
            status(text)
        end)
        kast.world.listCharacters()
    end,
    update = function(dt)
        if not entering then return end
        entering = entering + dt
        if entering >= ENTER_TIMEOUT then stopEntering("Couldn't enter the world. Try again.") end
    end,
}
