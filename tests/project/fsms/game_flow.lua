-- Game state machine (listed in project.kast "stateMachines"): one per Play session, kept across kast.loadScene.
return {
    name = "game_flow",
    initial = "boot",
    states = {
        boot = {
            enter = function(ctx) kast.log("game_flow: booting") end,
        },
        playing = {
            allowedFrom = {"boot", "paused"},
            enter = function(ctx) kast.log("game_flow: " .. ctx.from .. " -> playing") end,
        },
        paused = { allowedFrom = {"playing"} },
    },
}
