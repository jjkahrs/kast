-- Shared by client and server (see components/health.lua): the world server loads LUA_ROOT/items/ (kast.items, loot
-- tables in npcs/ name these ids) and the editor loads the project's items/ (kast.items, the bag and loot windows).
-- quality is WoW's: poor grey, common white, uncommon green. sellPrice is only shown (no vendors yet).
return {
    id = "bat_wing",
    name = "Bat Wing",
    icon = "textures/icons/bat_wing.png",
    quality = "poor",
    maxStack = 20,
    sellPrice = 1,
    description = "A leathery wing. Smells of the Cavern.",
}
