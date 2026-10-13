-- Drives the sample state machines: game_flow goes boot -> playing on the first frame; once it is playing, each traffic
-- light steps once (frame 10) and then tries to switch to red (frame 20), which only a yellow light may do: the green
-- one is denied (a Console warning, no change).
local following = {red = "green", green = "yellow", yellow = "red"}
local frame = 0

return {
    update = function(dt)
        frame = frame + 1
        if frame == 1 then
            kast.fsm.change("game_flow", "playing")
        end
        if kast.fsm.state("game_flow") ~= "playing" then
            return
        end
        for e in kast.query("StateMachine") do
            if frame == 10 then
                kast.fsm.change(e, following[kast.fsm.state(e)])
            elseif frame == 20 then
                local from = kast.fsm.state(e)
                local changed = kast.fsm.change(e, "red")
                kast.log(string.format("entity %d: %s -> red %s", e, from, changed and "allowed" or "denied"))
            end
        end
    end,
}
