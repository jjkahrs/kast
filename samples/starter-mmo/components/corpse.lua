-- Shared by client and server (see health.lua). A dead NPC that still holds loot (systems/inventory.lua adds it at
-- death, the corpse goes when emptied or after 60 s): owner is the entity id of the player who hit it first, the only
-- one who may loot it. Everyone in view gets it, so every client tips the corpse over; only the owner's shows the
-- sparkle (scripts/loot.lua). What the corpse holds is never replicated: only the owner gets it, by request.
return {
    name = "Corpse",
    replicate = "aoi",
    fields = {
        { name = "owner", type = "int", default = 0 },
    },
}
