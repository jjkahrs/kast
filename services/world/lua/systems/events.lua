-- Sample server system: the event bus and client events. On login the player gets "welcome" (kast.sendTo) and
-- "player.entered" goes on the bus. Its subscriber runs on the next tick, after the AOI step has sent the newcomer's
-- Spawn, and tells the newcomer and everyone who sees it "player.arrived" (kast.sendNearby). On logout the whole zone
-- gets "player.left" (kast.broadcast), before the leaver's Despawn.
--
-- Bandwidth: each client event is a message per recipient, and broadcast reaches every player in the zone. So this
-- sends only on login and logout, never per tick: in a crowd of N that is about N messages per login or logout.
return {
    init = function()
        kast.subscribe("player.entered", function(p)
            kast.sendNearby(p.entity, "player.arrived", {entity = p.entity, name = p.name})
        end)
    end,

    onEnter = function(e)
        local name = kast.get(e, "Character").name
        kast.sendTo(e, "welcome", {name = name})
        kast.publish("player.entered", {entity = e, name = name})
    end,

    onLeave = function(e)
        kast.broadcast("player.left", {entity = e, name = kast.get(e, "Character").name})
    end,

    update = function(dt) end,
}
