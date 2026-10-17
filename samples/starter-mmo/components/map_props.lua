-- Client-side tag: the scene props that belong to one map (portal arches, rocks). scripts/world_flow.lua shows the group
-- whose map the player is on and parks the others out of sight. The world server loads it too (it shares components/)
-- but never uses it: no persist, no replicate.
return {
    name = "MapProps",
    displayName = "Map Props",
    fields = {
        { name = "map", type = "int", default = 0, min = 0 },
    },
}
