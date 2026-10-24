-- SDFGI: flies the Main Camera along a fixed path (sdfgi_outdoor.scene), so the cascades scroll every few frames: a
-- slow arc 12 m to the right and back, 4 m forward, at most ~7 m/s, its pose a function of Play time alone (with
-- --fixed-dt, of the frame).
local t, camera = 0, nil

return {
    update = function(dt)
        t = t + dt
        if not camera then
            for e in kast.query("Camera") do camera = e end
        end
        local x = 250 + 12 * math.sin(t * 0.6)
        local z = 266 - 4 * (1 - math.cos(t * 0.6))
        kast.set(camera, "Transform", {position = {x = x, y = 31, z = z}})
    end,
}
