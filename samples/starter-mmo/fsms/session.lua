-- Game state machine (project.kast "stateMachines"): why the player is on the login screen. It survives
-- kast.loadScene, so scripts/world_flow.lua can tell scripts/login.lua why it sent the player back. No callbacks.
-- The world server loads fsms/ too but only runs the machines GAME_STATE_MACHINES lists, so this one stays client side.
return {
    name = "session",
    initial = "fresh",
    states = {
        fresh = {},        -- Play just started, or the player logged out
        disconnected = {}, -- the world server connection dropped
        expired = {},      -- the auth session could not be refreshed
    },
}
