-- Greys out an entity's material (its own instance) when a "health.empty" event names it.
return {
    init = function()
        kast.subscribe("health.empty", function(event)
            kast.log("health.empty: " .. event.title)
            if kast.has(event.id, "MeshRenderer") then
                kast.setMaterialProperty(event.id, "baseColor", {0.25, 0.25, 0.25, 1})
            end
        end)
    end,
    update = function(dt) end,
}
