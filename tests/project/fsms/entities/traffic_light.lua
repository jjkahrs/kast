-- Entity state machine (StateMachine component): red -> green -> yellow -> red, or "off" from anywhere. Entering a
-- state recolors the entity's material (its own instance).
local colors = {
    red = {0.9, 0.1, 0.1, 1},
    green = {0.1, 0.85, 0.2, 1},
    yellow = {0.95, 0.8, 0.1, 1},
    off = {0.2, 0.2, 0.2, 1},
}

local function show(ctx)
    if ctx.entity and kast.has(ctx.entity, "MeshRenderer") then
        kast.setMaterialProperty(ctx.entity, "baseColor", colors[ctx.to])
    end
end

return {
    name = "traffic_light",
    initial = "red",
    states = {
        red = { allowedFrom = {"yellow", "off"}, enter = show },
        green = { allowedFrom = {"red"}, enter = show },
        yellow = { allowedFrom = {"green"}, enter = show },
        off = { allowedFrom = {"*"}, enter = show },
    },
}
