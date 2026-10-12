-- Lifts every cube 1 m per second, up to 2.5 m.
local speed = 1.0
local top = 2.5

return {
    init = function() kast.log("lifting cubes") end,
    update = function(dt)
        for e in kast.query("Transform", "MeshRenderer") do
            if kast.get(e, "MeshRenderer").primitive == "cube" then
                local p = kast.get(e, "Transform").position
                p.y = math.min(p.y + speed * dt, top)
                kast.set(e, "Transform", {position = p})
            end
        end
    end,
}
