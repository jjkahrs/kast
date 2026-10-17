-- Logs the physics events (see docs/engine/physics.html) and, at init, a raycast straight down onto the ground.
local function logEvent(name)
    kast.subscribe(name, function(e)
        if e.normal then
            kast.log(string.format("%s a=%d b=%d normal=(%.2f, %.2f, %.2f)", name, e.a, e.b, e.normal.x, e.normal.y,
                                   e.normal.z))
        else
            kast.log(string.format("%s a=%d b=%d", name, e.a, e.b))
        end
    end)
end

return {
    init = function()
        for _, name in ipairs({"collisionEnter", "collisionExit", "triggerEnter", "triggerExit"}) do
            logEvent(name)
        end
        local hit = kast.physics.raycast({x = 4, y = 10, z = 4}, {x = 0, y = -1, z = 0}, 100)
        if hit then
            kast.log(string.format("raycast hit entity %d at y=%.2f, distance %.2f", hit.entity, hit.point.y,
                                   hit.distance))
        else
            kast.log("raycast hit nothing")
        end
    end,
    update = function(dt) end,
}
