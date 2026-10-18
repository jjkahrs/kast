-- Shared by client and server (see health.lua). The bag: 16 slots as two parallel arrays (component fields can't hold
-- tables): items[i] is an items/ id ("" = empty), counts[i] its stack size (0 when empty). Only the server changes it
-- (systems/inventory.lua); the owner's client shows it (scripts/bag.lua). Owner-only, saved per character.
-- Bandwidth: sent whole (16 short strings + 16 ints, well under 1 KB) on each change, i.e. once per bag action.
local EMPTY_ITEMS, EMPTY_COUNTS = {}, {}
for i = 1, 16 do EMPTY_ITEMS[i], EMPTY_COUNTS[i] = "", 0 end

return {
    name = "Inventory",
    persist = true,
    replicate = "owner",
    fields = {
        { name = "items",  type = "array", of = "string", default = EMPTY_ITEMS },
        { name = "counts", type = "array", of = "int",    default = EMPTY_COUNTS, min = 0 },
    },
}
