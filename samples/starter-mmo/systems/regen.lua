-- Server system: every character gets Health and a Wallet (components/, shared with the client's HUD). Health
-- regenerates toward max once the character has neither dealt nor taken a hit for OUT_OF_COMBAT seconds
-- ("combat.engaged" from systems/combat.lua), and never while it is dead (0 Health). Each login pays one gold, so a
-- relog shows the Wallet persisted.
--
-- Bandwidth: every change of a replicated component is sent in full to the owner and everyone in view
-- (docs/services/world/protocol.html#lua-components). So this runs once a second per character, each on its own phase (no burst tick), and keeps
-- Health in whole points, written only when it changes: at most one update a second per character, not one a tick.
local PERIOD = 1             -- seconds between updates of one character
local REGEN_PER_SECOND = 2
local OUT_OF_COMBAT = 5      -- seconds without a hit before regeneration starts

local state = {} -- entity id -> {wait = seconds to its next update, calm = seconds since its last hit}

return {
    init = function()
        kast.subscribe("combat.engaged", function(e)
            if state[e.entity] then state[e.entity].calm = 0 end
        end)
    end,

    onEnter = function(e)
        if not kast.has(e, "Health") then kast.add(e, "Health") end
        if not kast.has(e, "Wallet") then kast.add(e, "Wallet") end
        kast.set(e, "Wallet", {gold = kast.get(e, "Wallet").gold + 1})
        state[e] = {wait = (e % 20) / 20 * PERIOD, calm = OUT_OF_COMBAT} -- spread over the ticks of a period
    end,

    onLeave = function(e)
        state[e] = nil
    end,

    update = function(dt)
        for e, s in pairs(state) do
            s.calm = s.calm + dt
            s.wait = s.wait - dt
            if s.wait <= 0 then
                s.wait = s.wait + PERIOD
                local health = kast.get(e, "Health")
                if health.regen and health.current > 0 and s.calm >= OUT_OF_COMBAT then
                    local current = math.floor(math.min(health.max, health.current + REGEN_PER_SECOND * PERIOD) + 0.5)
                    if current ~= health.current then kast.set(e, "Health", {current = current}) end
                end
            end
        end
    end,
}
