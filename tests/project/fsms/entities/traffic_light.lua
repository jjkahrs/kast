-- Entity state machine (StateMachine component): red -> green -> yellow -> red, or "off" from anywhere. Entering a
-- state recolors the entity's MeshRenderer.
local colors = {
    red = {x = 0.9, y = 0.1, z = 0.1},
    green = {x = 0.1, y = 0.85, z = 0.2},
    yellow = {x = 0.95, y = 0.8, z = 0.1},
    off = {x = 0.2, y = 0.2, z = 0.2},
}

local function show(ctx)
    if ctx.entity and kast.has(ctx.entity, "MeshRenderer") then
        kast.set(ctx.entity, "MeshRenderer", {color = colors[ctx.to]})
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
