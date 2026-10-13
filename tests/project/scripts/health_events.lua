-- Publishes events and changes nothing itself: a "heartbeat" every 20 frames, and "health.empty" for every entity
-- with Health on frame 30. scripts/on_health_empty.lua reacts to the latter.
local frame = 0

return {
    update = function(dt)
        frame = frame + 1
        if frame % 20 == 0 then
            kast.publish("heartbeat", {frame = frame})
        end
        if frame == 30 then
            for e in kast.query("Health") do
                kast.publish("health.empty", {id = e, title = kast.get(e, "Health").title})
            end
        end
    end,
}
