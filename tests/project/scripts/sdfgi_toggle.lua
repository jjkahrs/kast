-- SDFGI: turns it off from Lua 0.2 s into Play (kast.sdfgi.set). The sdfgi_toggle_play shot must match the scene with
-- SDFGI off.
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            kast.sdfgi.set({enabled = false})
            assert(kast.sdfgi.get().enabled == false)
            kast.log("sdfgi toggled")
        end
    end,
}
