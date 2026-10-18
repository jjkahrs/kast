-- NPC machine, given to the patrolling Guard by its definition (npcs/guard.lua). walking <-> waiting is driven by
-- systems/npcs.lua; combat and returning by systems/combat.lua. Hit by a player, the Guard enters "combat" (from any
-- state but itself) and chases its attacker; leashed or left without a target it is "returning" to its spawn point,
-- and back there it heals and goes on "walking" its path. The systems do the moving; the machine only says which phase
-- the NPC is in, so each system can ask kast.fsm.state(npc) instead of keeping its own flag. At 0 Health it is "dead" (a
-- corpse that neither walks nor fights) until systems/inventory.lua removes it.
--
-- Bandwidth: no callbacks, so a transition sends nothing.
return {
    name = "patrol",
    initial = "walking",
    states = {
        walking = { allowedFrom = {"waiting", "returning"} },
        waiting = { allowedFrom = {"walking"} },
        combat = { allowedFrom = {"walking", "waiting", "returning"} },
        returning = { allowedFrom = {"combat"} },
        dead = { allowedFrom = {"walking", "waiting", "combat", "returning"} },
    },
}
