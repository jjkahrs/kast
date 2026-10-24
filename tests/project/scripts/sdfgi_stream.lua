-- SDFGI: widens the Terrain's streaming radius from 0 to 1 from Lua 0.5 s into Play, so the tiles around the camera's
-- stream in while it holds still; SDFGI must pick them up (sdfgi_stream shots).
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.5 then
            done = true
            for e in kast.query("Terrain") do kast.set(e, "Terrain", {radius = 1}) end
            kast.log("terrain radius 1")
        end
    end,
}
