-- Image-based lighting: recolors the sky from Lua 0.2 s into Play (kast.sky.set). The meshes' sky lighting must follow at
-- once: the ibl_sky_lua_play shot matches ibl_sky_recolored_play, ibl_sky.scene saved with these colors.
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            kast.sky.set({top = {x = 0.9, y = 0.2, z = 0.1}, bottom = {x = 0.1, y = 0.6, z = 0.2}})
            kast.log("sky recolored")
        end
    end,
}
