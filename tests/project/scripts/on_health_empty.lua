-- Greys out an entity's MeshRenderer when a "health.empty" event names it.
return {
    init = function()
        kast.subscribe("health.empty", function(event)
            kast.log("health.empty: " .. event.title)
            if kast.has(event.id, "MeshRenderer") then
                kast.set(event.id, "MeshRenderer", {color = {x = 0.25, y = 0.25, z = 0.25}})
            end
        end)
    end,
    update = function(dt) end,
}
