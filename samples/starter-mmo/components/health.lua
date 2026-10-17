-- Shared by client and server: the world server loads this folder as LUA_ROOT/components/ (docker-compose.override.yml)
-- and the editor loads it as the project's Lua components, so both sides agree on the fields. persist and replicate
-- are world-server keys (services/world/README.md "Component definitions"); the engine accepts and ignores them.
return {
    name = "Health",
    displayName = "Health",
    persist = true,    -- saved per character in MySQL
    replicate = "aoi", -- sent to the owner and everyone who can see the character
    fields = {
        { name = "current", type = "float", default = 100, min = 0 },
        { name = "max",     type = "float", default = 100, min = 1 },
        { name = "regen",   type = "bool",  default = true },
    },
}
