-- Tile scene streaming (docs/engine/terrain.html#tile-scenes) on scenes/tilegrid.scene (Terrain radius 0: only the
-- tile under the camera): logs every map.tileScene.loaded / unloaded event and moves the Main Camera across tiles by
-- frame count, so the editor's Play (editor_map_play_stream) and kast_player (player_tilegrid_stream) run the same path:
-- 0_0, then 1_0, then 1_1, then 2_0 (no scene; broken in the player test's copy), then back to 1_0.
local frame, camera = 0, nil
local path = {
    [30] = {x = 384, z = 128},
    [60] = {x = 384, z = 384},
    [80] = {x = 640, z = 128},
    [100] = {x = 384, z = 100},
}

local function logged(what)
    return function(event)
        kast.log(string.format("map stream: %s map %d tile %d,%d %s", what, event.map, event.tx, event.tz, event.path))
    end
end

return {
    init = function()
        kast.subscribe("map.tileScene.loaded", logged("loaded"))
        kast.subscribe("map.tileScene.unloaded", logged("unloaded"))
    end,
    update = function(dt)
        frame = frame + 1
        if not camera then
            for e in kast.query("Camera") do camera = e end
        end
        local to = path[frame]
        if to then
            kast.set(camera, "Transform", {position = {x = to.x, y = 90, z = to.z}})
        end
        if frame == 75 or frame == 115 then
            kast.log("map stream: scenes " .. table.concat(kast.loadedScenes(), ","))
        end
    end,
}
