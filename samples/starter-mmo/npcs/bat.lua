-- The Cavern's Bat (maps/cavern/.server/spawns/0_0.json). No path: it stays put until a player hits it; then the
-- creature machine (fsms/creature.lua) and systems/combat.lua make it fight back. Clients clone the scene entity "Bat"
-- under "NPC Templates".
return {
    name = "Bat",
    template = "Bat",
    components = {
        Health = { current = 10, max = 10, regen = false },
        StateMachine = { machine = "creature" },
    },
    -- Rolled once when it dies, for the player who hit it first (systems/inventory.lua); ids are items/ files.
    loot = {
        gold = { min = 1, max = 3 },
        items = {
            { item = "bat_wing",     chance = 0.8, min = 1, max = 1 },
            { item = "linen_cloth",  chance = 0.4, min = 1, max = 1 },
            { item = "worn_boots",   chance = 0.1, min = 1, max = 1 },
            { item = "cloth_gloves", chance = 0.1, min = 1, max = 1 },
        },
    },
}
