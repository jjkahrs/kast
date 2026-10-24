-- SDFGI: halves its energy from Lua 0.2 s into Play (kast.sdfgi.set). The sdfgi_lua_play shot must match
-- sdfgi_outdoor.scene saved with energy 0.5; the settings that rebuild the cascades are refused.
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            local before = kast.sdfgi.get()
            assert(before.enabled == true and before.energy == 1 and before.cascades == 4)
            kast.sdfgi.set({energy = 0.5})
            local ok = pcall(kast.sdfgi.set, {cellSize = 1})
            assert(not ok, "cellSize rebuilds the cascades: kast.sdfgi.set refuses it")
            ok = pcall(kast.sdfgi.set, {energy = -1})
            assert(not ok, "energy must be >= 0")
            assert(kast.sdfgi.get().energy == 0.5 and kast.sdfgi.get().cellSize == 0.5)
            kast.log("sdfgi set")
        end
    end,
}
