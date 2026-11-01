-- Zones (docs/engine/zones.html) on scenes/tilegrid.scene (maps/tilegrid: west x 0..1 with zones/west.scene, east x 2):
-- logs zone.left / zone.entered with kast.map.zone() and every map.zoneScene.loaded / unloaded event, and asks
-- kast.map.zoneAt() about points in each zone and off the map. map_stream.lua walks the Main Camera 0_0 -> 1_0 -> 1_1
-- -> 2_0 (into east at frame 80) -> 1_0 (back to west at frame 100); this script then steps it off the map at frame
-- 130. Run by the editor's Play (editor_map_play_zones) and kast_player (player_tilegrid_zones).
local frame, camera = 0, nil

local function name(zone)
    return zone and zone.id or "nil"
end

return {
    init = function()
        kast.subscribe("zone.left", function(event)
            kast.log(string.format("map zones: left map %d zone %s", event.map, event.zone))
        end)
        kast.subscribe("zone.entered", function(event)
            local zone = kast.map.zone()
            kast.log(string.format("map zones: entered map %d zone %s '%s' pvp=%s zone()=%s,%s,%s", event.map, event.zone,
                event.name, tostring(event.flags.pvp), name(zone), zone and zone.name, zone and tostring(zone.flags.pvp)))
        end)
        for _, what in ipairs({"loaded", "unloaded"}) do
            kast.subscribe("map.zoneScene." .. what, function(event)
                kast.log(string.format("map zones: scene %s map %d zone %s %s", what, event.map, event.zone, event.path))
            end)
        end
    end,
    update = function(dt)
        frame = frame + 1
        if not camera then
            for e in kast.query("Camera") do camera = e end
        end
        if frame == 5 then
            kast.log(string.format("map zones: zoneAt %s,%s,%s,%s", name(kast.map.zoneAt(10, 10)),
                name(kast.map.zoneAt(600, 700)), name(kast.map.zoneAt(-10, 10)), name(kast.map.zoneAt(900, 10))))
        elseif frame == 130 then
            kast.set(camera, "Transform", {position = {x = -300, y = 90, z = 128}})
        elseif frame == 145 then
            kast.log("map zones: off the map zone()=" .. name(kast.map.zone()))
        end
    end,
}
