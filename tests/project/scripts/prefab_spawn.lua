-- prefabs.scene: kast.instantiate at Play start (a crate in front of the others) and its failure case.
return {
    init = function()
        local id = kast.instantiate("prefabs/Crate.prefab", nil, {x = 0, y = 0.5, z = 3})
        kast.log("prefab_spawn: instantiated " .. tostring(id ~= nil))
        kast.log("prefab_spawn: missing gives " .. tostring(kast.instantiate("prefabs/nope.prefab")))
    end,
    update = function(dt) end,
}
