-- SSR: sets maxRoughness 0.25 and intensity 0.5 from Lua 0.2 s into Play (kast.ssr.set); bad input is refused whole. The
-- ssr_lua_play shot must match ssr.scene saved with those values (ssr_lua_ref, which runs this too: a no-op there).
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            local before = kast.ssr.get()
            assert(before.enabled == true and before.maxDistance == 50 and before.steps == 32)
            kast.ssr.set({maxRoughness = 0.25, intensity = 0.5})
            local ok, err = pcall(kast.ssr.set, {thickness = 1, steps = 200})
            assert(not ok and string.find(err, "steps", 1, true), "steps 200 is out of range")
            assert(not pcall(kast.ssr.set, {stride = 1}), "unknown field")
            assert(not pcall(kast.ssr.set, {steps = 16.5}), "steps must be an integer")
            assert(not pcall(kast.ssr.set, {intensity = 2}), "intensity is 0..1")
            assert(not pcall(kast.ssr.set, "on"), "expects a table")
            kast.ssr.set({steps = 64})
            assert(kast.ssr.get().steps == 64)
            kast.ssr.set({steps = 32})
            local after = kast.ssr.get()
            assert(after.enabled == true and after.maxRoughness == 0.25 and after.intensity == 0.5 and
                   math.abs(after.thickness - 0.3) < 1e-6 and after.maxDistance == 50 and after.steps == 32)
            kast.log("ssr set")
        end
    end,
}
