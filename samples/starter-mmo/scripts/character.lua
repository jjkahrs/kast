-- The character window (ui/character.rml), client side: a World of Warcraft-style paper doll. The server
-- (systems/inventory.lua) owns the gear; this shows the local player's replicated Equipment (components/equipment.lua)
-- and turns drags into requests:
--   C (or the "character.toggle" event) opens and closes it; not while typing in a GUI text field.
--   A bag slot dropped on an equipment slot (bag.slot.drop): kast.world.send("equipment.equip", {from, slot, item}).
--   Gear that doesn't fit that slot is refused here ("* Can't equip that there"), so the drop doesn't look accepted.
--   An equipment slot dropped on a bag slot (equip.slot.drop): "equipment.unequip" {slot, to, item}; on the bag's frame,
--   without to (the first empty slot). Elsewhere: nothing.
--   Hovering an equipment slot shows its tooltip: name in its quality colour, slot, stats, description.
-- The header shows the name and class; the stats Max Health (Health, as replicated: class base + 5 per Stamina) and the
-- gear's Armor and Damage. Which item fits which slot, and its stats, come from the server's "equipment.gear" on login.
-- Refusals are scripts/bag.lua's (every "inventory.rejected" goes into the chat box). Opening or closing publishes
-- "character.visibility" {open}; each stats change "character.stats" {maxHealth, armor, damage}.
--
-- Bandwidth: one ClientEvent per drop; the server answers with one Equipment and one Inventory update.
local SLOTS = 7

local me
local equipped = {}   -- equipment slot -> item id ("" = empty), as last replicated
local bagItems = {}   -- the bag's ids, as last replicated (the item a drop claims to move)
local gear = {gear = {}, names = {}, slots = {}} -- the server's "equipment.gear"
local open = false
local held = false    -- C down last frame
local hovered         -- equipment slot whose tooltip shows
local maxHealth = 0   -- Health.max, as last replicated

local function log(text) kast.publish("chat.local", {text = text}) end

local function equipOf(id) return tonumber(string.match(id or "", "^equip_(%d+)")) end -- also its icon / label
local function bagSlotOf(id) return tonumber(string.match(id or "", "^slot_(%d+)")) end

-- Gear's slot name and stats line ("+1 Armor  +2 Stamina"). scripts/bag.lua has the same (no shared functions).
local function gearLines(id)
    local g = gear.gear[id]
    if not g then return "", "" end
    local slotName = ""
    for i, slot in ipairs(gear.slots) do
        if slot == g.slot then slotName = gear.names[i] end
    end
    local stats = {}
    for _, stat in ipairs({{"armor", "Armor"}, {"damage", "Damage"}, {"stamina", "Stamina"}}) do
        if (g[stat[1]] or 0) > 0 then stats[#stats + 1] = "+" .. g[stat[1]] .. " " .. stat[2] end
    end
    return slotName, table.concat(stats, "  ")
end

local function drawSlot(i)
    local def = kast.items[equipped[i] or ""]
    local slot = "equip_" .. i
    kast.gui.setAttribute(slot, "class", def and ("slot equip q_" .. def.quality) or "slot equip empty")
    kast.gui.setAttribute(slot .. "_icon", "style", def and ("decorator: image(../" .. def.icon .. ")") or "decorator: none")
    kast.gui.setText(slot .. "_label", def and "" or (gear.names[i] or ""))
end

local function drawStats()
    local armor, damage = 0, 0
    for i = 1, SLOTS do
        local g = gear.gear[equipped[i] or ""]
        if g then armor, damage = armor + (g.armor or 0), damage + (g.damage or 0) end
    end
    kast.gui.setText("stat_health", maxHealth > 0 and string.format("Max Health %d", maxHealth) or "Max Health -")
    kast.gui.setText("stat_armor", "Armor " .. armor)
    kast.gui.setText("stat_damage", "Damage +" .. damage)
    kast.publish("character.stats", {maxHealth = maxHealth, armor = armor, damage = damage})
end

local function showTooltip(slot)
    hovered = slot
    local id = slot and equipped[slot] or ""
    local def = kast.items[id]
    kast.gui.setVisible("gear_tooltip", def ~= nil)
    if not def then return end
    local slotName, stats = gearLines(id)
    kast.gui.setText("gear_tooltip_name", def.name)
    kast.gui.setAttribute("gear_tooltip_name", "class", "q_" .. def.quality)
    kast.gui.setText("gear_tooltip_slot", slotName)
    kast.gui.setText("gear_tooltip_stats", stats)
    kast.gui.setText("gear_tooltip_description", def.description)
end

local function setOpen(value)
    if value ~= open then kast.publish("character.visibility", {open = value}) end
    open = value
    kast.gui.setVisible("character", open)
    if not open then showTooltip(nil) end
end

-- A bag slot dropped on this window: equip it if it was dropped on a slot it fits.
local function onBagDrop(e)
    if e.targetDocument ~= "ui/character.rml" then return end
    local from, slot = bagSlotOf(e.id), equipOf(e.target)
    local item = from and bagItems[from] or ""
    if not slot or item == "" then return end
    local g = gear.gear[item]
    if not g or g.slot ~= gear.slots[slot] then return log("* Can't equip that there") end
    kast.world.send("equipment.equip", {from = from, slot = slot, item = item})
end

local function onEquipDrop(e)
    local slot = equipOf(e.id)
    if not slot or (equipped[slot] or "") == "" or e.targetDocument ~= "ui/bag.rml" then return end
    kast.world.send("equipment.unequip", {slot = slot, to = bagSlotOf(e.target), item = equipped[slot]})
end

return {
    init = function()
        setOpen(false)
        kast.subscribe("world.entered", function(e) me = e.entityId end)
        -- replay: sent once on entering, maybe before this zone's scene loaded
        kast.subscribe("server.welcome", function(e) kast.gui.setText("character_name", e.name) end, {replay = true})
        kast.subscribe("server.equipment.gear", function(e)
            gear = e
            for i = 1, SLOTS do drawSlot(i) end
            drawStats()
        end, {replay = true})
        kast.subscribe("world.component", function(e)
            if e.entityId ~= me then return end
            if e.name == "Equipment" then
                for i = 1, SLOTS do
                    equipped[i] = e.data.items[i] or ""
                    drawSlot(i)
                end
                drawStats()
                if hovered then showTooltip(hovered) end
            elseif e.name == "Inventory" then
                for i, id in ipairs(e.data.items) do bagItems[i] = id end
            elseif e.name == "Health" and e.data.max ~= maxHealth then
                maxHealth = e.data.max
                drawStats()
            elseif e.name == "Class" then
                kast.gui.setText("character_class", e.data.id:sub(1, 1):upper() .. e.data.id:sub(2))
            end
        end)
        kast.subscribe("character.toggle", function() setOpen(not open) end)
        kast.subscribe("bag.slot.drop", onBagDrop)
        kast.subscribe("equip.slot.drop", onEquipDrop)
        kast.subscribe("world.disconnected", function() setOpen(false) end)
    end,

    update = function(dt)
        local inWorld = kast.world and kast.world.state() == "inWorld"
        local down = inWorld and not kast.gui.focusedField() and kast.input.keyDown("C")
        if down and not held then setOpen(not open) end
        held = down
        if open then
            local slot = equipOf(kast.gui.hovered())
            if slot ~= hovered then showTooltip(slot) end
        end
    end,
}
