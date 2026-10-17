-- The Cavern's Bat (maps/cavern/.server/spawns/0_0.json). No path and no machine: it stays put. Clients clone the scene
-- entity "Bat" under "NPC Templates".
return {
    name = "Bat",
    template = "Bat",
    components = {
        Health = { current = 10, max = 10, regen = false },
    },
}
