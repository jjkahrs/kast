-- Online Services sample: logs in through kast.auth, connects to a realm's world server through kast.world, makes (or
-- reuses) a character, enters the world and says hello. Needs a project with "online" enabled (README "Online
-- Services"); the tests run it against kast_net_test_server's fake auth and fake world (realm Testgrove).
local EMAIL, PASSWORD = "demo@example.com", "demo-password-123"
local REALM, CHARACTER = "Testgrove", "Demo"

return {
    init = function()
        if not kast.world then
            return kast.error("kast.world is nil: enable Online Services in project.kast")
        end
        kast.subscribe("auth.login.ok", function() kast.auth.realms() end)
        kast.subscribe("auth.realms", function(e)
            for _, realm in ipairs(e.realms) do
                if realm.name == REALM then
                    kast.log(string.format("connecting to %s (%s:%d)", realm.name, realm.host, realm.port))
                    return kast.world.connect(realm.id)
                end
            end
            kast.error("no realm named " .. REALM)
        end)
        kast.subscribe("world.connected", function(e)
            kast.log(string.format("world: connected as account %d (%s)", e.accountId, e.role))
            kast.world.listCharacters()
        end)
        kast.subscribe("world.charList", function(e)
            for _, character in ipairs(e.characters) do
                if character.name == CHARACTER then return kast.world.enterWorld(character.id) end
            end
            kast.world.createCharacter(CHARACTER)
        end)
        kast.subscribe("world.charCreated", function(e)
            kast.log(string.format("world: created %s (%d)", e.character.name, e.character.id))
            kast.world.enterWorld(e.character.id)
        end)
        kast.subscribe("world.entered", function(e)
            kast.log(string.format("world: entered as entity %d at %g, %g, %g", e.entityId, e.position.x,
                                   e.position.y, e.position.z))
            kast.world.say("hello world")
        end)
        kast.subscribe("world.spawn", function(e) kast.log("world: " .. e.name .. " is nearby") end)
        kast.subscribe("world.chat", function(e) kast.log("world: chat from " .. e.from .. ": " .. e.text) end)
        kast.subscribe("world.charError", function(e) kast.error("world: " .. e.request .. " failed: " .. e.code) end)
        kast.subscribe("world.disconnected", function(e) kast.warn("world: disconnected: " .. e.code) end)
        kast.subscribe("auth.error", function(e) kast.error(string.format("%s failed: %s", e.op, e.code)) end)
        kast.auth.login(EMAIL, PASSWORD)
    end,
    update = function(dt) end,
}
