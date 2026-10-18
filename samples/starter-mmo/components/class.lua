-- Shared by client and server (see health.lua): the character's class, picked on the Characters screen
-- (scripts/characters.lua) and set by the server's systems/classes.lua from the creation options. Saved with the
-- character; owner-only, since only the owner's HUD shows it.
return {
    name = "Class",
    persist = true,
    replicate = "owner",
    fields = {
        { name = "id", type = "enum", values = {"warrior", "mage", "priest", "rogue"}, default = "warrior" },
    },
}
