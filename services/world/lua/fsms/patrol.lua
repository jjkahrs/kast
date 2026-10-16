-- Sample NPC machine, attached by lua/systems/npcs.lua: walking <-> waiting. Each state may only be entered from the
-- other one. The system does the moving; the machine only says which phase the NPC is in, so another system (or a
-- later quest step) can ask kast.fsm.state(guard) instead of keeping its own flag.
--
-- Bandwidth: no callbacks, so a transition sends nothing.
return {
    name = "patrol",
    initial = "walking",
    states = {
        walking = { allowedFrom = {"waiting"} },
        waiting = { allowedFrom = {"walking"} },
    },
}
