-- Volumetric fog: thickens it (density 0.1), lowers its anisotropy to 0.3 and turns its temporal blend off from Lua 0.2 s
-- into Play (kast.volumetricFog.set); bad input is refused whole. The volfog_lua_play shot must match
-- volumetric_fog.scene saved with those values (volfog_lua_ref, which runs this too: a no-op there; with no history
-- left after the set, both frames are the same).
local t, done = 0, false

return {
    update = function(dt)
        t = t + dt
        if not done and t >= 0.2 then
            done = true
            local before = kast.volumetricFog.get()
            assert(before.enabled == true and before.maxDistance == 60 and before.lightIntensity == 1)
            kast.volumetricFog.set({density = 0.1, anisotropy = 0.3, temporalBlend = 0})
            local ok, err = pcall(kast.volumetricFog.set, {density = 0.2, anisotropy = 0.95})
            assert(not ok and string.find(err, "anisotropy", 1, true), "anisotropy 0.95 is out of range")
            assert(not pcall(kast.volumetricFog.set, {noise = 1}), "unknown field")
            assert(not pcall(kast.volumetricFog.set, {density = "thick"}), "density must be a number")
            assert(not pcall(kast.volumetricFog.set, {albedo = {1, 2, 1}}), "albedo is 0..1")
            assert(not pcall(kast.volumetricFog.set, {maxDistance = 0.5}), "maxDistance is 1..1000")
            assert(not pcall(kast.volumetricFog.set, "on"), "expects a table")
            local after = kast.volumetricFog.get()
            local function near(a, b) return math.abs(a - b) < 1e-6 end
            assert(after.enabled == true and near(after.density, 0.1) and near(after.anisotropy, 0.3) and
                   after.temporalBlend == 0 and after.maxDistance == 60 and near(after.albedo[1], 0.9))
            kast.log("volumetric fog set")
        end
    end,
}
