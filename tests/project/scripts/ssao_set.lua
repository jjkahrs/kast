-- SSAO: sets radius 2 and intensity 2 from Lua 0.2 s into Play (kast.ssao.set); bad input is refused whole. The
-- ssao_lua_play shot must match ssao.scene saved with those values (ssao_lua_ref, which runs this too: a no-op there).
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            local before = kast.ssao.get()
            assert(before.enabled == true and before.power == 1.5 and before.sliceCount == 2)
            kast.ssao.set({radius = 2, intensity = 2})
            local ok, err = pcall(kast.ssao.set, {radius = 3, sliceCount = 9})
            assert(not ok and string.find(err, "sliceCount", 1, true), "sliceCount 9 is out of range")
            assert(not pcall(kast.ssao.set, {bias = 1}), "unknown field")
            assert(not pcall(kast.ssao.set, {sliceCount = 2.5}), "sliceCount must be an integer")
            assert(not pcall(kast.ssao.set, 1), "expects a table")
            kast.ssao.set({sliceCount = 3})
            assert(kast.ssao.get().sliceCount == 3)
            kast.ssao.set({sliceCount = 2})
            local after = kast.ssao.get()
            assert(after.enabled == true and after.radius == 2 and after.intensity == 2 and after.power == 1.5 and
                   after.sliceCount == 2)
            kast.log("ssao set")
        end
    end,
}
