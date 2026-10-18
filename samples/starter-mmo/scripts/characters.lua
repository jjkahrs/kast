-- Character select (the "Characters" UIDocument of scenes/world.scene, ui/characters.rml): lists the account's
-- characters on the realm (the first four, "Play <Name> (<Class>)"), creates one, enters the world with the one
-- clicked. On world.entered it hides itself and shows the HUD and chat. The realm screen connected; the connection
-- survives kast.loadScene.
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

local function status(text) kast.gui.setText("characters_status", text) end

local function showDocuments(inWorld)
    for e in kast.query("UIDocument") do
        local path = kast.get(e, "UIDocument").path
        kast.set(e, "UIDocument", {visible = (path == "ui/characters.rml") ~= inWorld})
    end
end

return {
    init = function()
        for i = 1, MAX do kast.gui.setVisible("char_" .. i, false) end
        showDocuments(false)
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
            if not character then return end
            status("Entering the world as " .. character.name .. "...")
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
        kast.subscribe("world.charError", function(e) status(ERRORS[e.code] or e.code) end)
        kast.subscribe("world.entered", function() showDocuments(true) end)
        kast.world.listCharacters()
    end,
    update = function(dt) end,
}
