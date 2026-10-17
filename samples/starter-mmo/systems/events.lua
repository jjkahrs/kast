-- Server system: client events. On login the player gets "welcome" (kast.sendTo) and "player.entered" goes on the
-- server's bus; its subscriber runs on the next tick, after the newcomer's Spawn, and tells the newcomer and everyone who
-- sees it "player.arrived" (kast.sendNearby). The client shows both in the chat box (scripts/chat.lua).
--
-- Bandwidth: sends only on login, never per tick.
return {
    init = function()
        kast.subscribe("player.entered", function(p)
            kast.sendNearby(p.entity, "player.arrived", {name = p.name})
        end)
        kast.subscribe("area.enter", function(p)
            kast.log(string.format("area.enter entity %d map %d area %s", p.entity, p.map, p.area))
        end)
    end,

    onEnter = function(e)
        local name = kast.get(e, "Character").name
        kast.sendTo(e, "welcome", {name = name})
        kast.publish("player.entered", {entity = e, name = name})
    end,

    update = function(dt) end,
}
