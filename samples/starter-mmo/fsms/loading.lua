-- Game state machine (project.kast "stateMachines"): whether the player is entering the world from character select,
-- so the loading screen outlives kast.loadScene. scripts/characters.lua enters "entering" on the click (and goes back
-- to "idle" if entering fails); the zone scene's scripts/loading.lua keeps the screen up while it is "entering" and
-- sets it back once the zone renders. No callbacks.
return {
    name = "loading",
    initial = "idle",
    states = {
        idle = {},     -- not entering the world (Play in a zone, a portal, the menus)
        entering = {}, -- clicked a character, the zone not rendered yet
    },
}
