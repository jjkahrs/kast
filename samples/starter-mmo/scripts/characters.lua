-- Character select (the "Characters" UIDocument of scenes/world.scene, ui/characters.rml): lists the account's
-- characters on the realm (the first four), creates one, enters the world with the one clicked. On world.entered it
-- hides itself and shows the HUD and chat. The realm screen connected; the connection survives kast.loadScene.
local MAX = 4
local ERRORS = {
    name_taken = "That name is taken on this realm.",
    name_invalid = "Names are 3-16 letters (A-Z), no spaces or digits.",
    char_limit = "No more characters on this account.",
    rate_limited = "One request at a time, please.",
    service_unavailable = "The world server can't reach its database. Try again soon.",
}
local characters = {}

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
                if character then kast.gui.setText("char_" .. i, "Play " .. character.name) end
            end
            status(#characters == 0 and "Create your first character below." or "")
        end)
        kast.subscribe("characters.pick", function(e)
            local character = characters[tonumber(e.id:match("^char_(%d+)$"))]
            if not character then return end
            status("Entering the world as " .. character.name .. "...")
            kast.world.enterWorld(character.id)
        end)
        kast.subscribe("characters.create", function()
            status("Creating...")
            kast.world.createCharacter(kast.gui.value("char_name"))
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
