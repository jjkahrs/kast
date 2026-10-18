-- Server system: the Meadow Shrine, an interactable zone object (maps/meadow/objects/0_0.json, its server side in
-- maps/meadow/.server/interactables/0_0.json with event "shrine.activated"). The world server checks the map and the
-- range of an Interact and publishes the event {entity, map, object, data}; this restores a living character's Health
-- and tells it with the shrine's data.message (scripts/shrines.lua shows it in the chat box). The dead get nothing:
-- reviving stays systems/combat.lua's job.
return {
    init = function()
        kast.subscribe("shrine.activated", function(e)
            local health = kast.has(e.entity, "Health") and kast.get(e.entity, "Health")
            if not health or health.current <= 0 then return end
            if health.current < health.max then kast.set(e.entity, "Health", {current = health.max}) end
            kast.sendTo(e.entity, "shrine.blessing", {text = e.data and e.data.message or "The shrine hums."})
        end)
    end,
    update = function(dt) end,
}
