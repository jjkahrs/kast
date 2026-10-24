-- VoxelGI: halves every volume's light from Lua 0.2 s into Play (kast.set on its VoxelGI). The voxelgi_room_lua_play
-- shot must match voxelgi_room_lua_ref_play, voxelgi_room.scene saved with intensity 0.5.
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            for e in kast.query("VoxelGI") do
                local volume = kast.get(e, "VoxelGI")
                assert(volume.enabled == true and volume.intensity == 1)
                kast.set(e, "VoxelGI", {intensity = 0.5})
                assert(kast.get(e, "VoxelGI").intensity == 0.5)
            end
            kast.log("voxelgi set")
        end
    end,
}
