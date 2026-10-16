-- Sample server system: state machines. Every character gets the "locomotion" machine (fsms/locomotion.lua) in
-- onEnter, which enters "idle" at once. update moves it to "moving" when the character's Transform changes and back to
-- "idle" after it stood still for STILL_SECONDS (so a gap between two client moves is not a stop). The zone-wide game
-- machine "zone_cycle" (fsms/zone_cycle.lua) flips between day and night every PHASE_SECONDS, if GAME_STATE_MACHINES
-- lists it.
--
-- Bandwidth: the machine's enter sends one message to the owner per transition (a start or a stop), never per tick.
local STILL_SECONDS = 1
local PHASE_SECONDS = 300

local chars = {} -- entity -> { pos = last position, still = seconds without moving }
local zoneCycle = false
local phaseTime = 0

local function moved(a, b) return a.x ~= b.x or a.y ~= b.y or a.z ~= b.z end

return {
    init = function()
        -- kast.fsm raises for a game machine GAME_STATE_MACHINES doesn't list. Touching it here enters "day" now.
        zoneCycle = pcall(kast.fsm.state, "zone_cycle")
    end,

    onEnter = function(e)
        kast.add(e, "StateMachine")
        kast.set(e, "StateMachine", {machine = "locomotion", initialState = "idle"}) -- enters "idle" now
        -- Denied: "idle" may only be entered from "moving". Returns false and logs a warning; no enter, no event.
        kast.fsm.change(e, "idle")
        chars[e] = {pos = kast.get(e, "Transform").position, still = 0}
    end,

    onLeave = function(e) chars[e] = nil end, -- the machine goes away with the character, without an exit

    update = function(dt)
        for e, c in pairs(chars) do
            local pos = kast.get(e, "Transform").position
            if moved(pos, c.pos) then
                c.still = 0
                if kast.fsm.state(e) == "idle" then kast.fsm.change(e, "moving") end
            else
                c.still = c.still + dt
                if c.still >= STILL_SECONDS and kast.fsm.state(e) == "moving" then kast.fsm.change(e, "idle") end
            end
            c.pos = pos
        end

        if zoneCycle then
            phaseTime = phaseTime + dt
            if phaseTime >= PHASE_SECONDS then
                phaseTime = 0
                kast.fsm.change("zone_cycle", kast.fsm.state("zone_cycle") == "day" and "night" or "day")
            end
        end
    end,
}
