-- VoxelGI: turns every volume off from Lua 0.2 s into Play (kast.set on its VoxelGI). The voxelgi_room_toggle_play shot
-- must match voxelgi_room_disabled_play, voxelgi_room.scene saved with the volume disabled.
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            for e in kast.query("VoxelGI") do
                kast.set(e, "VoxelGI", {enabled = false})
                assert(kast.get(e, "VoxelGI").enabled == false)
            end
            kast.log("voxelgi toggled")
        end
    end,
}
