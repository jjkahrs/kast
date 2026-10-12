-- Fades every sphere from its color to green over two seconds.
local elapsed = 0
local green = {x = 0.2, y = 0.85, z = 0.3}

return {
    update = function(dt)
        elapsed = elapsed + dt
        local t = math.min(elapsed / 2, 1)
        for e in kast.query("MeshRenderer") do
            local mesh = kast.get(e, "MeshRenderer")
            if mesh.primitive == "sphere" then
                local c = mesh.color
                kast.set(e, "MeshRenderer", {color = {
                    x = c.x + (green.x - c.x) * t,
                    y = c.y + (green.y - c.y) * t,
                    z = c.z + (green.z - c.z) * t,
                }})
            end
        end
    end,
    shutdown = function() kast.log("tint done") end,
}
