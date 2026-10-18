-- Sample test hooks for the bot. Remove this file from a real game's lua/systems/.
-- Scenario 18 ("respawn"): a character whose name starts with "Respawntest" destroys the sample Guard (spawn
-- "guard_1", maps/start/.server/spawns/4_4.json) when it enters, so the bot can watch it respawn respawnSec later.
-- Scenarios 21 and 22 ("client-events", "client-event-rate"): every "client.test.echo" event (kast.world.send's
-- ClientEvent) goes back to its sender as "test.echo" {n = data.n, entity = sender}.
-- Scenario 25 ("objects"): the hills' Test Shrine (maps/hills/.server/interactables/0_1.json) publishes "test.shrine"
-- when a player activates it in range; it goes back to that player with what the server put in the event.
return {
    init = function()
        kast.subscribe("client.test.echo", function(e)
            local n = type(e.data.n) == "number" and e.data.n or -1 -- data is the client's: never trusted
            kast.sendTo(e.entity, "test.echo", {n = n, entity = e.entity})
        end)
        kast.subscribe("test.shrine", function(e)
            kast.sendTo(e.entity, "test.shrine", {entity = e.entity, map = e.map, object = e.object, msg = e.data and e.data.msg})
        end)
    end,
    onEnter = function(e)
        if kast.get(e, "Character").name:sub(1, 11) ~= "Respawntest" then return end
        for npc in kast.query("Spawn") do
            if kast.get(npc, "Spawn").id == "guard_1" then kast.destroy(npc) end
        end
    end,
    update = function() end,
}
