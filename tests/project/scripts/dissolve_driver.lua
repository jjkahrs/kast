-- Animates the dissolve sample (custom_shaders.scene, and its graph copy shader_graphs.scene): every mesh using one of
-- `materials` dissolves and re-forms over `period` seconds by setting its shader's threshold param. The change is per
-- entity (a material instance) and is gone on Stop.
local materials = { ["materials/shader_dissolve.mat"] = true, ["materials/graph_dissolve.mat"] = true }
local period = 6
local elapsed = 0

return {
    update = function(dt)
        elapsed = elapsed + dt
        local threshold = 0.5 - 0.5 * math.cos(2 * math.pi * elapsed / period)
        for e in kast.query("MeshRenderer") do
            if materials[kast.get(e, "MeshRenderer").material] then
                kast.setMaterialProperty(e, "threshold", threshold)
            end
        end
    end,
}
