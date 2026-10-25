-- Volumetric fog: sweeps the Main Camera sideways (volumetric_fog.scene), 3 m either way at up to 4.5 m/s, its pose a
-- function of Play time alone (with --fixed-dt, of the frame), so the temporal reprojection's ghosting shows if any.
local t, camera = 0, nil

return {
    update = function(dt)
        t = t + dt
        if not camera then
            for e in kast.query("Camera") do camera = e end
        end
        kast.set(camera, "Transform", {position = {x = 3 * math.sin(t * 1.5), y = 3, z = 15}})
    end,
}
