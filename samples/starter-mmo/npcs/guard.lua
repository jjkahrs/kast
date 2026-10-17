-- The Meadow's Guard (maps/meadow/.server/spawns/0_0.json). It patrols its spawn's path: systems/npcs.lua moves it, the
-- patrol machine (fsms/patrol.lua) says whether it walks or waits. Clients clone the scene entity "Guard" under
-- "NPC Templates" (scenes/world.scene).
return {
    name = "Guard",
    template = "Guard",
    components = {
        Health = { current = 50, max = 50 },
        StateMachine = { machine = "patrol" },
    },
}
