-- Sample test hook for the bot (scenario 18, "respawn"): a character whose name starts with "Respawntest" destroys the
-- sample Guard (spawn "guard_1", maps/start/.server/spawns/4_4.json) when it enters, so the bot can watch it respawn
-- respawnSec later. Remove this file from a real game's lua/systems/.
return {
    onEnter = function(e)
        if kast.get(e, "Character").name:sub(1, 11) ~= "Respawntest" then return end
        for npc in kast.query("Spawn") do
            if kast.get(npc, "Spawn").id == "guard_1" then kast.destroy(npc) end
        end
    end,
    update = function() end,
}
