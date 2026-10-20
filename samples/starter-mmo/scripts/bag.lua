-- The bag window (ui/bag.rml), client side. The server (systems/inventory.lua) owns the bag; this shows the local
-- player's replicated Inventory (components/inventory.lua) and turns drags into requests:
--   B (or the "bag.toggle" event) opens and closes it; not while typing in a GUI text field.
--   Drag a stack onto another slot: kast.world.send("inventory.move", {from, to, item}) — the server moves it to an
--   empty slot, merges it into the same item (up to maxStack) or swaps it with another. With Shift onto an empty slot,
--   split = true: half the stack moves. Dropped outside the bag (the world, another window): a confirmation dialog,
--   then "inventory.destroy" {slot, item}. item is the id this client shows in that slot, so a stale view is refused.
--   Dropped on the character window (ui/character.rml): scripts/character.lua equips it (or not); never destroyed.
--   Hovering a slot shows its tooltip: name in its quality colour, count, slot and stats for gear (the server's
--   "equipment.gear"), description, sell price.
-- Slots show the item's icon (kast.items[id].icon), a quality-coloured border and the count when over 1.
-- Opening or closing publishes "bag.visibility" {open}.
-- Every "inventory.rejected" {reason} the server sends (bag, loot and equipment requests) goes into the chat box
-- ("chat.local").
--
-- Bandwidth: one ClientEvent per drop or confirmed destroy; the server answers with one Inventory update.
local SLOTS = 16
local REJECTED = {
    slot = "Invalid slot",
    empty = "That slot is empty",
    item = "Your bag changed, try again",
    split = "Can't split that",
    stack_full = "That stack is full",
    full = "Inventory is full",
    not_owner = "You can't loot that",
    no_loot = "Nothing to loot",
    range = "Too far away",
    index = "That is gone",
    dead = "You are dead",
    wrong_slot = "Can't equip that there",
    combat = "Can't do that in combat",
    occupied = "That bag slot is taken",
}

local me                      -- the local player's server entity id
local items, counts = {}, {}  -- the bag as last replicated
local shown = {}              -- slot -> "item:count" drawn, to write only changed slots
local open = false
local held = false            -- B down last frame
local hovered                 -- slot whose tooltip shows
local doomed                  -- {slot, item} waiting for the destroy confirmation
local gear = {gear = {}, names = {}, slots = {}} -- the server's "equipment.gear"

local function log(text) kast.publish("chat.local", {text = text}) end

local function slotOf(id) return tonumber(string.match(id or "", "^slot_(%d+)")) end -- also its icon / count

-- Gear's slot name and stats line ("+1 Armor  +2 Stamina"), or "" for other items. scripts/character.lua has the same
-- (client scripts can't share functions).
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
    local item, count = items[i] or "", counts[i] or 0
    local key = item .. ":" .. count
    if shown[i] == key then return end
    shown[i] = key
    local def = kast.items[item]
    local slot = "slot_" .. i
    kast.gui.setAttribute(slot, "class", def and ("slot q_" .. def.quality) or "slot empty")
    kast.gui.setAttribute(slot .. "_icon", "style", def and ("decorator: image(../" .. def.icon .. ")") or "decorator: none")
    kast.gui.setText(slot .. "_count", def and count > 1 and tostring(count) or "")
end

local function showTooltip(slot)
    hovered = slot
    local def = slot and kast.items[items[slot] or ""]
    kast.gui.setVisible("bag_tooltip", def ~= nil)
    if not def then return end
    kast.gui.setText("tooltip_name", def.name)
    kast.gui.setAttribute("tooltip_name", "class", "q_" .. def.quality)
    kast.gui.setText("tooltip_count", counts[slot] > 1 and ("Stack of " .. counts[slot]) or "")
    local slotName, stats = gearLines(items[slot])
    kast.gui.setText("tooltip_slot", slotName)
    kast.gui.setText("tooltip_stats", stats)
    kast.gui.setText("tooltip_description", def.description)
    kast.gui.setText("tooltip_price", string.format("Sell price: %d gold", def.sellPrice))
end

local function closeDialog()
    doomed = nil
    kast.gui.setVisible("destroy_dialog", false)
end

local function setOpen(value)
    if value ~= open then kast.publish("bag.visibility", {open = value}) end
    open = value
    kast.gui.setVisible("bag", open)
    if not open then
        showTooltip(nil)
        closeDialog()
    end
end

local function onDrop(e)
    local from = slotOf(e.id)
    if not from or (items[from] or "") == "" then return end
    if e.targetDocument == "ui/character.rml" then return end -- scripts/character.lua's
    if e.target ~= "" and e.targetDocument == e.document then
        local to = slotOf(e.target)
        if not to or to == from then return end -- the bag's frame or title: nothing
        local split = e.shift and (items[to] or "") == "" or nil
        kast.world.send("inventory.move", {from = from, to = to, item = items[from], split = split})
        return
    end
    doomed = {slot = from, item = items[from]}
    local def = kast.items[items[from]]
    kast.gui.setText("destroy_text", string.format("Destroy %s%s?", def and def.name or items[from],
        counts[from] > 1 and (" x" .. counts[from]) or ""))
    kast.gui.setVisible("destroy_dialog", true)
end

return {
    init = function()
        setOpen(false)
        kast.subscribe("world.entered", function(e) me = e.entityId end)
        kast.subscribe("world.component", function(e)
            if e.entityId ~= me or e.name ~= "Inventory" then return end
            for i = 1, SLOTS do
                items[i], counts[i] = e.data.items[i] or "", e.data.counts[i] or 0
                drawSlot(i)
            end
            if hovered then showTooltip(hovered) end
            if doomed and items[doomed.slot] ~= doomed.item then closeDialog() end
        end)
        kast.subscribe("bag.toggle", function() setOpen(not open) end)
        kast.subscribe("bag.slot.drop", onDrop)
        kast.subscribe("server.equipment.gear", function(e) gear = e end, {replay = true}) -- replay: sent once on entering, maybe before this zone's scene loaded
        kast.subscribe("bag.destroy_confirm", function()
            if doomed then kast.world.send("inventory.destroy", doomed) end
            closeDialog()
        end)
        kast.subscribe("bag.destroy_cancel", closeDialog)
        kast.subscribe("server.inventory.rejected", function(e) log("* " .. (REJECTED[e.reason] or e.reason)) end)
        kast.subscribe("world.disconnected", function() setOpen(false) end)
    end,

    update = function(dt)
        local inWorld = kast.world and kast.world.state() == "inWorld"
        local down = inWorld and not kast.gui.focusedField() and kast.input.keyDown("B")
        if down and not held then setOpen(not open) end
        held = down
        if open then
            local slot = slotOf(kast.gui.hovered())
            if slot ~= hovered then showTooltip(slot) end
        end
    end,
}
