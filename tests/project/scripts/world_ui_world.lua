-- Drives world_ui_world.scene (ctest world_ui_world_*, docs/engine/world-ui.html), by Play time (with --fixed-dt
-- 1/60: frames): before 1 s the camera A at (0, 1.5, 6) looks down -Z and Fixed is unrotated; from 1 s Fixed is
-- turned 60 degrees around Y; from 2 s the camera B looks at the Sign from 45 degrees to its right, from 3 s the
-- camera C from 45 degrees above, both 5 m from it, as A.
local t, camera, fixed = 0, nil, nil

return {
    init = function()
        for e in kast.query("Camera") do camera = e end
        for e in kast.query("WorldUI") do
            if kast.get(e, "WorldUI").facing == "fixed" then fixed = e end
        end
        kast.log((camera and fixed) and "world_ui_world: PASS" or "world_ui_world: FAIL fixture entities not found")
    end,
    update = function(dt)
        t = t + dt
        local s, c = math.sin(math.rad(22.5)), math.cos(math.rad(22.5))
        if t >= 1 then
            kast.set(fixed, "Transform", {rotation = {x = 0, y = 0.5, z = 0, w = math.cos(math.rad(30))}})
        end
        if t >= 3 then
            kast.set(camera, "Transform", {position = {x = 0, y = 1.5 + 5 * math.sqrt(0.5), z = 1 + 5 * math.sqrt(0.5)},
                                           rotation = {x = -s, y = 0, z = 0, w = c}})
        elseif t >= 2 then
            kast.set(camera, "Transform", {position = {x = 5 * math.sqrt(0.5), y = 1.5, z = 1 + 5 * math.sqrt(0.5)},
                                           rotation = {x = 0, y = s, z = 0, w = c}})
        end
    end,
}
