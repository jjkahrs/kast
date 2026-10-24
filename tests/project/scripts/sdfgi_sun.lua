-- SDFGI: swings the sun 60 degrees around the vertical from Lua 0.5 s into Play (kast.set on its Transform), at the
-- same height. The bounced light SDFGI gives the overhang's underside (no direct sun either way) must follow.
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.5 then
            done = true
            for e in kast.query("DirectionalLight") do
                local r = kast.get(e, "Transform").rotation
                -- turn by 60 degrees about world y: q = (0, sin 30, 0, cos 30) * r
                local s, c = 0.5, math.sqrt(3) / 2
                kast.set(e, "Transform", {rotation = {x = c * r.x + s * r.z, y = c * r.y + s * r.w,
                                                      z = c * r.z - s * r.x, w = c * r.w - s * r.y}})
            end
            kast.log("sun moved")
        end
    end,
}
