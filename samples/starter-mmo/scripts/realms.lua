-- Realm screen (scenes/realms.scene, ui/realms.rml): lists kast.auth.realms() as buttons (the first four), connects to
-- the one clicked, and on world.connected goes on to scenes/characters.scene, where the character is chosen.
local MAX = 4
local realms = {}

local function status(text) kast.gui.setText("realms_status", text) end

return {
    init = function()
        local account = kast.auth and kast.auth.account()
        if not account then return kast.loadScene("scenes/login.scene") end
        kast.gui.setText("account", "Logged in as " .. account.email)
        for i = 1, MAX do kast.gui.setVisible("realm_" .. i, false) end
        status("Loading realms...")
        kast.subscribe("auth.realms", function(e)
            realms = e.realms
            for i = 1, MAX do
                local realm = realms[i]
                kast.gui.setVisible("realm_" .. i, realm ~= nil)
                if realm then kast.gui.setText("realm_" .. i, realm.name .. "  (" .. realm.status .. ")") end
            end
            status(#realms == 0 and "No realms yet: run the realm setup (README)." or "")
        end)
        kast.subscribe("realms.pick", function(e)
            local realm = realms[tonumber(e.id:match("^realm_(%d+)$"))]
            if not realm or kast.world.state() ~= "disconnected" then return end
            status("Connecting to " .. realm.name .. "...")
            kast.world.connect(realm.id)
        end)
        kast.subscribe("world.connected", function() kast.loadScene("scenes/characters.scene") end)
        kast.subscribe("world.disconnected", function(e) status("Could not connect: " .. e.code) end)
        kast.subscribe("realms.logout", function()
            kast.auth.logout()
            kast.loadScene("scenes/login.scene")
        end)
        kast.subscribe("auth.error", function(e) status(e.op .. " failed: " .. e.code) end)
        kast.auth.realms()
    end,
    update = function(dt) end,
}
