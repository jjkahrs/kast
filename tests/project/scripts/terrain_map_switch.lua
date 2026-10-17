-- Terrain sample: what Replication does on world.mapChanged, offline. After 0.5 s the Terrain's map becomes 3 (the
-- hills) and the hero is placed above the ground there (as the server places a player at a portal's destination); the
-- streamer swaps the tiles on the next frame. Used by the terrain_switch screenshot tests (docs/engine/terrain.html).
local t, switched, reported = 0, false, false

return {
    update = function(dt)
        t = t + dt
        if not switched and t >= 0.5 then
            switched = true
            for e in kast.query("Terrain") do kast.set(e, "Terrain", {map = 3}) end
            for e in kast.query("CharacterController") do kast.set(e, "Transform", {position = {x = 240, y = 28, z = 236}}) end
            kast.log("terrain switch: map 1 -> 3")
        elseif switched and not reported and t >= 1.0 then
            reported = true
            local tiles = 0
            for e in kast.query("TerrainTile") do
                if kast.get(e, "TerrainTile").map == 3 then tiles = tiles + 1 end
            end
            kast.log("terrain switch: " .. tiles .. " tiles on map 3")
        end
    end,
}
