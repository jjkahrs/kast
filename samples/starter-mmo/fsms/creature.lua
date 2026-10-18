-- NPC machine for a creature that stands still until attacked, given to the Bat by its definition (npcs/bat.lua) and
-- driven by systems/combat.lua: idle -> combat when a player hits it, combat -> returning when it is leashed or loses
-- its target, returning -> idle back at its spawn point (healed), or straight back into combat if hit on the way. At 0 Health it
-- is "dead" (a corpse) until systems/inventory.lua removes it.
--
-- Bandwidth: no callbacks, so a transition sends nothing.
return {
    name = "creature",
    initial = "idle",
    states = {
        idle = { allowedFrom = {"returning"} },
        combat = { allowedFrom = {"idle", "returning"} },
        returning = { allowedFrom = {"combat"} },
        dead = { allowedFrom = {"idle", "combat", "returning"} },
    },
}
