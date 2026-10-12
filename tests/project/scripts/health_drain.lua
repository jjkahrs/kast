-- Drains Health over time and shows it: MeshRenderer.color fades from Health.tint (full) to dark grey (empty).
-- Entities with regen refill once empty.
local drainPerSecond = 40

return {
    update = function(dt)
        for e in kast.query("Health", "MeshRenderer") do
            local h = kast.get(e, "Health")
            local current = math.max(0, h.current - drainPerSecond * dt)
            if current == 0 and h.regen then
                current = h.max
            end
            kast.set(e, "Health", {current = current})
            local f = current / h.max
            local dark = 0.15 * (1 - f)
            kast.set(e, "MeshRenderer", {color = {x = h.tint.x * f + dark, y = h.tint.y * f + dark, z = h.tint.z * f + dark}})
        end
    end,
}
