-- Animates the dissolve sample (custom_shaders.scene): every mesh using materials/shader_dissolve.mat dissolves and
-- re-forms over `period` seconds by setting its .kshader's threshold param. The change is per entity (a material
-- instance) and is gone on Stop.
local material = "materials/shader_dissolve.mat"
local period = 6
local elapsed = 0

return {
    update = function(dt)
        elapsed = elapsed + dt
        local threshold = 0.5 - 0.5 * math.cos(2 * math.pi * elapsed / period)
        for e in kast.query("MeshRenderer") do
            if kast.get(e, "MeshRenderer").material == material then
                kast.setMaterialProperty(e, "threshold", threshold)
            end
        end
    end,
}
