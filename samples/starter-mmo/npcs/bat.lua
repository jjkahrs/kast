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
}
