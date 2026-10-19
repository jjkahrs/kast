-- Shared by client and server (see health.lua). What the character wears: 7 slots as an array of items/ ids ("" =
-- empty), in this fixed order: 1 Head, 2 Chest, 3 Legs, 4 Feet, 5 Hands, 6 Main Hand, 7 Off Hand. Which item fits
-- which slot, and its Armor / Damage / Stamina, is the server's GEAR table (systems/inventory.lua), sent to the owner
-- as "equipment.gear". Only the server changes it (systems/inventory.lua); the owner's client shows it
-- (scripts/character.lua). Owner-only, saved per character.
-- Bandwidth: sent whole (7 short strings) on each change, i.e. once per equip or unequip.
local EMPTY = {}
for i = 1, 7 do EMPTY[i] = "" end

return {
    name = "Equipment",
    persist = true,
    replicate = "owner",
    fields = {
        { name = "items", type = "array", of = "string", default = EMPTY },
    },
}
