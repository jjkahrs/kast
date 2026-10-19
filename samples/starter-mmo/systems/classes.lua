-- Server system: classes. The client creates a character with kast.world.createCharacter(name, {class = <id>})
-- (scripts/characters.lua); the server's C++ checks the options' size and characters, then asks validateCharacter
-- below, which accepts exactly one option, class, naming one of CLASSES (anything else: CharError options_invalid).
-- On entering the world the character gets Class (components/class.lua) from its creation options, unless it has a
-- saved one; a character created before classes has neither and becomes a DEFAULT, saved like any Class. Its class sets
-- Health.max, plus STAMINA_HEALTH per point of its gear's Stamina ("equipment.stats" {entity, stamina, ...} from
-- systems/inventory.lua, on entering and after each gear change): a new character starts full, a saved one keeps its
-- Health (at most the max). Equipping raises the max but not the current Health; unequipping lowers the max and clamps
-- the current Health to it. systems/combat.lua revives at Health.max and gives each class its powers. This runs before
-- combat.lua and regen.lua (systems run in filename order).
--
-- Bandwidth: Class is sent to the owner once, on entering; Health as usual (systems/regen.lua), plus once per gear change.
local CLASSES = {
    warrior = {maxHealth = 140},
    mage = {maxHealth = 80},
    priest = {maxHealth = 90},
    rogue = {maxHealth = 110},
}
local DEFAULT = "warrior"
local STAMINA_HEALTH = 5 -- max Health per point of gear Stamina

local saved = {} -- player -> its saved current Health, until its first "equipment.stats" sets the max with gear

local function exists(e, component)
    local ok, has = pcall(kast.has, e, component) -- raises for ids that are not in the world
    return ok and has
end

local function onGearStats(e)
    if not exists(e.entity, "Character") then return end
    local max = CLASSES[kast.get(e.entity, "Class").id].maxHealth + STAMINA_HEALTH * e.stamina
    local current = math.min(saved[e.entity] or kast.get(e.entity, "Health").current, max)
    saved[e.entity] = nil
    kast.set(e.entity, "Health", {max = max, current = current})
end

return {
    validateCharacter = function(name, options)
        for key in pairs(options) do
            if key ~= "class" then return false end
        end
        return CLASSES[options.class] ~= nil
    end,

    init = function()
        kast.subscribe("equipment.stats", onGearStats)
    end,

    onEnter = function(e)
        if not kast.has(e, "Class") then
            local wanted = kast.get(e, "Character").options.class
            kast.add(e, "Class")
            kast.set(e, "Class", {id = CLASSES[wanted] and wanted or DEFAULT})
        end
        local max = CLASSES[kast.get(e, "Class").id].maxHealth
        if kast.has(e, "Health") then
            saved[e] = kast.get(e, "Health").current -- the max may be higher with gear (onGearStats)
            kast.set(e, "Health", {max = max, current = math.min(saved[e], max)})
        else
            kast.add(e, "Health")
            kast.set(e, "Health", {max = max, current = max})
        end
    end,

    onLeave = function(e)
        saved[e] = nil
    end,

    update = function(dt) end,
}
