-- Lightmaps: halves the baked light and turns the directional term off from Lua 0.2 s into Play (kast.lightmaps.set).
-- The lightmap_lua_play shot must match lightmap_room_lua_ref_play, lightmap_room.scene saved with those settings.
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            local before = kast.lightmaps.get()
            assert(before.enabled == true and before.intensity == 1 and before.directional == true)
            kast.lightmaps.set({intensity = 0.5, directional = false})
            local ok = pcall(kast.lightmaps.set, {bounces = 4})
            assert(not ok, "bounces need a re-bake: kast.lightmaps.set refuses them")
            assert(kast.lightmaps.get().intensity == 0.5 and kast.lightmaps.get().bounces == 2)
            kast.log("lightmaps set")
        end
    end,
}
