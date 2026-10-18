-- Server system: classes. The client creates a character with kast.world.createCharacter(name, {class = <id>})
-- (scripts/characters.lua); the server's C++ checks the options' size and characters, then asks validateCharacter
-- below, which accepts exactly one option, class, naming one of CLASSES (anything else: CharError options_invalid).
-- On entering the world the character gets Class (components/class.lua) from its creation options, unless it has a
-- saved one; a character created before classes has neither and becomes a DEFAULT, saved like any Class. Its class sets
-- Health.max: a new character starts full, a saved one keeps its Health (at most the max). systems/combat.lua revives at
-- Health.max and gives each class its powers. This runs before combat.lua and regen.lua (systems run in filename order).
--
-- Bandwidth: Class is sent to the owner once, on entering; Health as usual (systems/regen.lua).
local CLASSES = {
    warrior = {maxHealth = 140},
    mage = {maxHealth = 80},
    priest = {maxHealth = 90},
    rogue = {maxHealth = 110},
}
local DEFAULT = "warrior"

return {
    validateCharacter = function(name, options)
        for key in pairs(options) do
            if key ~= "class" then return false end
        end
        return CLASSES[options.class] ~= nil
    end,

    onEnter = function(e)
        if not kast.has(e, "Class") then
            local wanted = kast.get(e, "Character").options.class
            kast.add(e, "Class")
            kast.set(e, "Class", {id = CLASSES[wanted] and wanted or DEFAULT})
        end
        local max = CLASSES[kast.get(e, "Class").id].maxHealth
        if kast.has(e, "Health") then
            kast.set(e, "Health", {max = max, current = math.min(kast.get(e, "Health").current, max)})
        else
            kast.add(e, "Health")
            kast.set(e, "Health", {max = max, current = max})
        end
    end,

    update = function(dt) end,
}
