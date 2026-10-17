-- Online Services NPC sample (docs/engine/online-services.html#replication): Play with the editor's test login (Window >
-- Online > Test login, or --online-login-env) enters the world as "Watcher" on the realm "Kast Live". The world
-- server's sample lua/systems/npcs.lua patrols a "Guard" NPC around the spawn point; replication shows it as a clone of
-- the "Guard" entity under "NPC Templates" (NetworkReplication.npcTemplates) and remote players as clones of
-- "Remote Player". Needs a project with "online" enabled whose caFile trusts the world server's certificate.
local REALM, CHARACTER = "Kast Live", "Watcher"

return {
    init = function()
        if not kast.world then
            return kast.error("kast.world is nil: enable Online Services in project.kast")
        end
        kast.subscribe("auth.session", function() kast.auth.realms() end)
        kast.subscribe("auth.realms", function(e)
            for _, realm in ipairs(e.realms) do
                if realm.name == REALM then return kast.world.connect(realm.id) end
            end
            kast.error("no realm named " .. REALM)
        end)
        kast.subscribe("world.connected", function() kast.world.listCharacters() end)
        kast.subscribe("world.charList", function(e)
            for _, character in ipairs(e.characters) do
                if character.name == CHARACTER then return kast.world.enterWorld(character.id) end
            end
            kast.world.createCharacter(CHARACTER)
        end)
        kast.subscribe("world.charCreated", function(e) kast.world.enterWorld(e.character.id) end)
        kast.subscribe("world.spawn", function(e)
            kast.log(string.format("npcs: spawn %s (%s, template '%s') at %g, %g", e.name, tostring(e.kind),
                                   tostring(e.template), e.position.x, e.position.z))
        end)
        kast.subscribe("world.entitySpawned", function(e)
            kast.log(string.format("npcs: %s is entity %d", e.name, e.entity))
        end)
        kast.subscribe("auth.error", function(e) kast.error(string.format("%s failed: %s", e.op, e.code)) end)
        kast.subscribe("world.error", function(e) kast.error("world: " .. tostring(e.code)) end)
    end,
    update = function() end,
}
