-- Depth of field: pulls focus from 6 m to 14 m at f/2 from Lua 0.2 s into Play (kast.dof.set, a cutscene's focus pull);
-- bad input is refused whole. The dof_lua_play shot must match dof.scene saved with those values (dof_lua_ref, which
-- runs this too: a no-op there).
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            local before = kast.dof.get()
            assert(before.enabled == true and before.focalLength == 85 and before.maxCoc == 24)
            kast.dof.set({focusDistance = 14, fStop = 2})
            local ok, err = pcall(kast.dof.set, {focusDistance = 3, fStop = 0.2})
            assert(not ok and string.find(err, "fStop", 1, true), "fStop 0.2 is out of range")
            assert(not pcall(kast.dof.set, {autofocus = true}), "unknown field")
            assert(not pcall(kast.dof.set, {maxCoc = 100}), "maxCoc is 1..64")
            assert(not pcall(kast.dof.set, {focalLength = "85mm"}), "focalLength must be a number")
            assert(not pcall(kast.dof.set, "on"), "expects a table")
            local after = kast.dof.get()
            assert(after.enabled == true and after.focusDistance == 14 and after.fStop == 2 and after.focalLength == 85 and
                   after.maxCoc == 24)
            kast.log("dof set")
        end
    end,
}
