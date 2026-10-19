-- The Meadow's Guard (maps/meadow/.server/spawns/0_0.json). It patrols its spawn's path: systems/npcs.lua moves it, the
-- patrol machine (fsms/patrol.lua) says whether it walks, waits, fights (systems/combat.lua) or returns. Clients clone
-- the scene entity "Guard" under "NPC Templates" (scenes/world.scene).
return {
    name = "Guard",
    template = "Guard",
    components = {
        Health = { current = 50, max = 50 },
        StateMachine = { machine = "patrol" },
    },
    -- Rolled once when it dies, for the player who hit it first (systems/inventory.lua); ids are items/ files.
    loot = {
        gold = { min = 5, max = 15 },
        items = {
            { item = "linen_cloth",          chance = 0.6,  min = 1, max = 2 },
            { item = "minor_healing_potion", chance = 0.25, min = 1, max = 1 },
            { item = "worn_shortsword",      chance = 0.1,  min = 1, max = 1 },
            { item = "leather_cap",          chance = 0.1,  min = 1, max = 1 },
            { item = "linen_tunic",          chance = 0.1,  min = 1, max = 1 },
            { item = "patched_trousers",     chance = 0.1,  min = 1, max = 1 },
            { item = "wooden_shield",        chance = 0.05, min = 1, max = 1 },
        },
    },
}
