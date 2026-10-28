-- Server system: the bag (components/inventory.lua) and NPC loot. The server decides every change; clients only ask,
-- with kast.world.send, and each request arrives here as "client.<name>" {entity, data}. data is untrusted: anything
-- malformed or not allowed changes nothing, and the requester alone gets "inventory.rejected" {reason}.
--
-- Bag requests (slots are 1..SLOTS; item is the id the client believes is in the from slot, so a stale view is caught):
--   "inventory.move"    {from, to, item, split}  to an empty slot: moves the stack (split = true: moves half of it,
--                        floor(n/2), leaving the rest); onto the same item: merges up to its maxStack, the remainder
--                        stays in from; onto another item: swaps the two.
--   "inventory.destroy" {slot, item}            destroys the stack.
--
-- Equipment (components/equipment.lua, slots 1..7 = EQUIP_SLOTS) lives here too, since equipping moves items between
-- it and the bag in one step (and system files can't share readBag / writeBag). GEAR says which items/ ids are gear,
-- their slot and their flat stats; the owner gets it on entering as "equipment.gear" {slots, names, gear}. Both
-- requests are refused while dead ("dead") or within OUT_OF_COMBAT seconds of a hit ("combat", "combat.engaged" from
-- systems/combat.lua, the same window systems/regen.lua uses):
--   "equipment.equip"   {from, slot, item}  bag slot from -> equipment slot; the item's GEAR slot must be that slot
--                        ("wrong_slot"). Whatever was equipped there goes into bag slot from (a swap).
--   "equipment.unequip" {slot, to?, item}   equipment slot -> bag slot to: an empty slot takes it; gear for the same
--                        equipment slot swaps with it; any other item refuses it ("occupied", or "full" when the bag
--                        has no empty slot). No to: the first empty bag slot, or "full".
-- After each change, and on entering, "equipment.stats" {entity, armor, damage, stamina} (the gear's totals) goes on the
-- server bus: systems/combat.lua applies Armor and Damage, systems/classes.lua Stamina's max Health.
--
-- Loot: "npc.died" {npc, tagger} (systems/combat.lua) rolls the NPC definition's loot table (npcs/*.lua, kast.npcDef)
-- once: gold between gold.min and gold.max, and each item with its chance, min..max of it. Loot belongs to the tagger
-- alone. The corpse gets Corpse {owner = tagger} (replicated to everyone in view, so clients tip it over and the owner's
-- shows a sparkle) and stays until it is emptied or CORPSE_SECONDS pass; then it is destroyed (its spawn file brings
-- the NPC back respawnSec later). Nothing to loot (or no tagger): destroyed at once.
--   "loot.open"     {target}         the owner, alive, on the corpse's map within LOOT_RANGE: gets "loot.contents"
--                                    {target, gold, items = {{item, count}, ...}}.
--   "loot.take"     {target, index}  index "gold" or 1..#items: takes that entry.
--   "loot.take_all" {target}         takes everything.
-- Gold goes to Wallet.gold. Items go into the bag, first onto stacks of the same item (up to maxStack), then into empty
-- slots; what doesn't fit stays in the corpse and the owner gets "inventory.rejected" {reason = "full"}. After a take
-- the owner gets "loot.contents" again, or "loot.closed" {target} when the corpse is empty and goes (also when it
-- times out).
--
-- Other systems change the bag through the server bus, so the bag rules stay here:
--   "inventory.exchange" {entity, ref, take = {{item, count}, ...}?, give = {{item, count}, ...}?, gold?} takes the
--   items out of the bag (any slots), then puts the given ones in (stacks first, then empty slots) and adds gold to
--   the Wallet, all or nothing; answered on the bus with "inventory.exchanged" {entity, ref, ok, reason?}: reason
--   "missing" (not enough to take) or "full" (what is given doesn't fit after the take). systems/quests.lua turns in
--   quests with it.
--   Every bag write publishes "inventory.changed" {entity} (systems/quests.lua's collect objectives).
--
-- Bandwidth: one event per request back to the requester only, plus the Inventory / Wallet / Equipment change (owner
-- only) and the Corpse component once per death (everyone in view). "equipment.gear" once per login. Nothing per tick.
local SLOTS = 16
local LOOT_RANGE = 5        -- metres, on x/z
local CORPSE_SECONDS = 60   -- a corpse with loot left goes after this
local OUT_OF_COMBAT = 5     -- seconds after a hit dealt or taken before gear may change (systems/regen.lua's too)
local EQUIP_SLOTS = {"head", "chest", "legs", "feet", "hands", "main_hand", "off_hand"} -- components/equipment.lua
local SLOT_NAMES = {"Head", "Chest", "Legs", "Feet", "Hands", "Main Hand", "Off Hand"}
local GEAR = { -- items/ id -> its slot and stats (missing = 0); gear items have maxStack 1
    leather_cap      = {slot = "head",      armor = 1, stamina = 1},
    linen_tunic      = {slot = "chest",     armor = 2, stamina = 2},
    patched_trousers = {slot = "legs",      armor = 1, stamina = 1},
    worn_boots       = {slot = "feet",      armor = 1, stamina = 1},
    cloth_gloves     = {slot = "hands",     armor = 1, stamina = 1},
    worn_shortsword  = {slot = "main_hand", damage = 3},
    wooden_shield    = {slot = "off_hand",  armor = 2, stamina = 1},
}
-- ponytail: math.random's sequence starts the same on every server start (the sandbox has no clock to seed it from);
-- fine for a sample, seed it from a real source in a game where drops matter.

local now = 0
local corpses = {} -- npc -> {owner = player, gold = n, items = {{item =, count =}, ...}, despawnAt = time}
local lastHit = {} -- player -> time of its last "combat.engaged"

local function exists(e, component)
    local ok, has = pcall(kast.has, e, component) -- raises for ids that are not in the world
    return ok and has
end

local function reject(e, reason)
    kast.sendTo(e, "inventory.rejected", {reason = reason})
end

local function slotIndex(v)
    return type(v) == "number" and v == math.floor(v) and v >= 1 and v <= SLOTS and v or nil
end

-- The bag as two plain arrays of exactly SLOTS entries: missing slots empty, unknown items and bad counts dropped.
local function readBag(e)
    local inv = kast.get(e, "Inventory")
    local items, counts = {}, {}
    for i = 1, SLOTS do
        local id, n = inv.items[i] or "", inv.counts[i] or 0
        local def = kast.items[id]
        if def and n >= 1 then
            items[i], counts[i] = id, math.min(n, def.maxStack)
        else
            items[i], counts[i] = "", 0
        end
    end
    return items, counts
end

local function writeBag(e, items, counts)
    kast.set(e, "Inventory", {items = items, counts = counts})
    kast.publish("inventory.changed", {entity = e})
end

-- Puts up to count of item into the bag arrays: same-item stacks first, then empty slots. Returns what didn't fit.
local function addTo(items, counts, item, count)
    local maxStack = kast.items[item].maxStack
    for i = 1, SLOTS do
        if count == 0 then return 0 end
        if items[i] == item and counts[i] < maxStack then
            local n = math.min(count, maxStack - counts[i])
            counts[i], count = counts[i] + n, count - n
        end
    end
    for i = 1, SLOTS do
        if count == 0 then return 0 end
        if items[i] == "" then
            local n = math.min(count, maxStack)
            items[i], counts[i], count = item, n, count - n
        end
    end
    return count
end

-- Takes count of item out of the bag arrays, last slots first. Returns false (arrays partly changed) if there is less.
local function removeFrom(items, counts, item, count)
    for i = SLOTS, 1, -1 do
        if count == 0 then return true end
        if items[i] == item then
            local n = math.min(count, counts[i])
            counts[i], count = counts[i] - n, count - n
            if counts[i] == 0 then items[i] = "" end
        end
    end
    return count == 0
end

local function onExchange(event)
    local e = event.entity
    local function answer(reason)
        kast.publish("inventory.exchanged", {entity = e, ref = event.ref, ok = reason == nil, reason = reason})
    end
    if not exists(e, "Inventory") then return answer("missing") end -- logged out meanwhile
    local items, counts = readBag(e)
    for _, entry in ipairs(event.take or {}) do
        if not removeFrom(items, counts, entry.item, entry.count) then return answer("missing") end
    end
    for _, entry in ipairs(event.give or {}) do
        if addTo(items, counts, entry.item, entry.count) > 0 then return answer("full") end
    end
    writeBag(e, items, counts)
    if (event.gold or 0) > 0 then kast.set(e, "Wallet", {gold = kast.get(e, "Wallet").gold + event.gold}) end
    answer(nil)
end

local function onMove(event)
    local e, data = event.entity, event.data
    if type(data) ~= "table" then return reject(e, "slot") end
    local from, to = slotIndex(data.from), slotIndex(data.to)
    if not from or not to or from == to then return reject(e, "slot") end
    local items, counts = readBag(e)
    if items[from] == "" then return reject(e, "empty") end
    if data.item ~= items[from] then return reject(e, "item") end
    local item, n = items[from], counts[from]
    if data.split == true then
        if items[to] ~= "" or n < 2 then return reject(e, "split") end
        local half = math.floor(n / 2)
        items[to], counts[to], counts[from] = item, half, n - half
    elseif items[to] == "" then
        items[to], counts[to], items[from], counts[from] = item, n, "", 0
    elseif items[to] == item then
        local moved = math.min(n, kast.items[item].maxStack - counts[to])
        if moved == 0 then return reject(e, "stack_full") end
        counts[to], counts[from] = counts[to] + moved, n - moved
        if counts[from] == 0 then items[from] = "" end
    else
        items[from], counts[from], items[to], counts[to] = items[to], counts[to], item, n
    end
    writeBag(e, items, counts)
end

local function onDestroy(event)
    local e, data = event.entity, event.data
    local slot = type(data) == "table" and slotIndex(data.slot) or nil
    if not slot then return reject(e, "slot") end
    local items, counts = readBag(e)
    if items[slot] == "" then return reject(e, "empty") end
    if data.item ~= items[slot] then return reject(e, "item") end
    items[slot], counts[slot] = "", 0
    writeBag(e, items, counts)
end

local function equipIndex(v)
    return type(v) == "number" and v == math.floor(v) and v >= 1 and v <= #EQUIP_SLOTS and v or nil
end

local function fits(item, slot) return GEAR[item] ~= nil and GEAR[item].slot == EQUIP_SLOTS[slot] end

-- The equipment as a plain array of #EQUIP_SLOTS ids: unknown items and items in the wrong slot dropped.
local function readGear(e)
    local saved = kast.get(e, "Equipment").items
    local gear = {}
    for i = 1, #EQUIP_SLOTS do
        local id = saved[i] or ""
        gear[i] = kast.items[id] and fits(id, i) and id or ""
    end
    return gear
end

local function publishStats(e, gear)
    local total = {entity = e, armor = 0, damage = 0, stamina = 0}
    for _, id in ipairs(gear) do
        local def = GEAR[id]
        if def then
            total.armor = total.armor + (def.armor or 0)
            total.damage = total.damage + (def.damage or 0)
            total.stamina = total.stamina + (def.stamina or 0)
        end
    end
    kast.publish("equipment.stats", total)
end

local function writeGear(e, gear)
    kast.set(e, "Equipment", {items = gear})
    publishStats(e, gear)
end

-- Why e may not change its gear now, or nil.
local function gearProblem(e)
    if kast.get(e, "Health").current <= 0 then return "dead" end
    if lastHit[e] and now - lastHit[e] < OUT_OF_COMBAT then return "combat" end
end

local function onEquip(event)
    local e, data = event.entity, event.data
    local from = type(data) == "table" and slotIndex(data.from) or nil
    local slot = from and equipIndex(data.slot) or nil
    if not slot then return reject(e, "slot") end
    local problem = gearProblem(e)
    if problem then return reject(e, problem) end
    local items, counts = readBag(e)
    if items[from] == "" then return reject(e, "empty") end
    if data.item ~= items[from] then return reject(e, "item") end
    if not fits(items[from], slot) then return reject(e, "wrong_slot") end
    local gear = readGear(e)
    local old = gear[slot]
    gear[slot] = items[from]
    items[from], counts[from] = old, old == "" and 0 or 1
    writeBag(e, items, counts)
    writeGear(e, gear)
end

local function onUnequip(event)
    local e, data = event.entity, event.data
    local slot = type(data) == "table" and equipIndex(data.slot) or nil
    local to = slot and slotIndex(data.to)
    if not slot or (data.to ~= nil and not to) then return reject(e, "slot") end
    local problem = gearProblem(e)
    if problem then return reject(e, problem) end
    local gear = readGear(e)
    if gear[slot] == "" then return reject(e, "empty") end
    if data.item ~= gear[slot] then return reject(e, "item") end
    local items, counts = readBag(e)
    local free
    for i = 1, SLOTS do
        if items[i] == "" then free = free or i end
    end
    to = to or free
    if not to then return reject(e, "full") end
    local swap = items[to]
    if swap ~= "" and not fits(swap, slot) then return reject(e, free and "occupied" or "full") end
    items[to], counts[to], gear[slot] = gear[slot], 1, swap
    writeBag(e, items, counts)
    writeGear(e, gear)
end

local function roll(npc)
    local def = kast.npcDef(kast.get(npc, "Npc").npc)
    local loot = def and def.loot or {}
    local gold = loot.gold and math.random(loot.gold.min, loot.gold.max) or 0
    local items = {}
    for _, entry in ipairs(loot.items or {}) do
        if math.random() < entry.chance then
            items[#items + 1] = {item = entry.item, count = math.random(entry.min, entry.max)}
        end
    end
    return gold, items
end

local function onDied(event)
    local npc, tagger = event.npc, event.tagger
    if not exists(npc, "Npc") then return end
    local gold, items = roll(npc)
    if not tagger or (gold == 0 and #items == 0) then return kast.destroy(npc) end
    corpses[npc] = {owner = tagger, gold = gold, items = items, despawnAt = now + CORPSE_SECONDS}
    kast.add(npc, "Corpse")
    kast.set(npc, "Corpse", {owner = tagger})
end

local function removeCorpse(npc)
    local corpse = corpses[npc]
    corpses[npc] = nil
    if exists(corpse.owner, "Character") then kast.sendTo(corpse.owner, "loot.closed", {target = npc}) end
    if exists(npc, "Npc") then kast.destroy(npc) end
end

local function contents(npc, corpse)
    kast.sendTo(corpse.owner, "loot.contents", {target = npc, gold = corpse.gold, items = corpse.items})
end

-- The corpse e asks about, if e may loot it now; else rejects and returns nil.
local function lootable(e, data)
    local npc = type(data) == "table" and data.target or nil
    local corpse = type(npc) == "number" and corpses[npc] or nil
    if not corpse then return reject(e, "no_loot") end
    if corpse.owner ~= e then return reject(e, "not_owner") end
    if kast.get(e, "Health").current <= 0 then return reject(e, "dead") end
    local from, to = kast.get(e, "Transform"), kast.get(npc, "Transform")
    local dx, dz = from.position.x - to.position.x, from.position.z - to.position.z
    if from.map ~= to.map or math.sqrt(dx * dx + dz * dz) > LOOT_RANGE then return reject(e, "range") end
    return npc, corpse
end

-- Moves the chosen entries (indices into corpse.items, and gold if takeGold) to e.
local function take(e, npc, corpse, takeGold, indices)
    if takeGold and corpse.gold > 0 then
        kast.set(e, "Wallet", {gold = kast.get(e, "Wallet").gold + corpse.gold})
        corpse.gold = 0
    end
    if #indices > 0 then
        local items, counts = readBag(e)
        local full = false
        for _, i in ipairs(indices) do
            local entry = corpse.items[i]
            entry.count = addTo(items, counts, entry.item, entry.count)
            full = full or entry.count > 0
        end
        writeBag(e, items, counts)
        local left = {}
        for _, entry in ipairs(corpse.items) do
            if entry.count > 0 then left[#left + 1] = entry end
        end
        corpse.items = left
        if full then reject(e, "full") end
    end
    if corpse.gold == 0 and #corpse.items == 0 then return removeCorpse(npc) end
    contents(npc, corpse)
end

local function onLootOpen(event)
    local npc, corpse = lootable(event.entity, event.data)
    if npc then contents(npc, corpse) end
end

local function onLootTake(event)
    local e = event.entity
    local npc, corpse = lootable(e, event.data)
    if not npc then return end
    local index = event.data.index
    if index == "gold" then
        if corpse.gold == 0 then return reject(e, "index") end
        return take(e, npc, corpse, true, {})
    end
    if type(index) ~= "number" or index ~= math.floor(index) or not corpse.items[index] then return reject(e, "index") end
    take(e, npc, corpse, false, {index})
end

local function onLootTakeAll(event)
    local npc, corpse = lootable(event.entity, event.data)
    if not npc then return end
    local all = {}
    for i = 1, #corpse.items do all[i] = i end
    take(event.entity, npc, corpse, true, all)
end

return {
    init = function()
        kast.subscribe("client.inventory.move", onMove)
        kast.subscribe("client.inventory.destroy", onDestroy)
        kast.subscribe("client.equipment.equip", onEquip)
        kast.subscribe("client.equipment.unequip", onUnequip)
        kast.subscribe("combat.engaged", function(e) lastHit[e.entity] = now end)
        kast.subscribe("npc.died", onDied)
        kast.subscribe("client.loot.open", onLootOpen)
        kast.subscribe("client.loot.take", onLootTake)
        kast.subscribe("client.loot.take_all", onLootTakeAll)
        kast.subscribe("inventory.exchange", onExchange)
    end,

    onEnter = function(e)
        if not kast.has(e, "Inventory") then kast.add(e, "Inventory") end
        writeBag(e, readBag(e)) -- a saved bag from an older items/ set: unknown items dropped, always SLOTS slots
        if not kast.has(e, "Equipment") then kast.add(e, "Equipment") end
        writeGear(e, readGear(e)) -- the same for saved gear, and items no longer in their slot
        kast.sendTo(e, "equipment.gear", {slots = EQUIP_SLOTS, names = SLOT_NAMES, gear = GEAR})
    end,

    onLeave = function(e)
        lastHit[e] = nil
    end,

    -- Each map has its own Lua state (docs/services/world/maps.html#map-contexts): the time since the last hit goes
    -- with the character (each map counts its own now).
    onMapLeave = function(e)
        local since = lastHit[e] and now - lastHit[e]
        lastHit[e] = nil
        return {sinceHit = since}
    end,
    onMapEnter = function(e, carried)
        if carried and carried.sinceHit then lastHit[e] = now - carried.sinceHit end
    end,

    update = function(dt)
        now = now + dt
        for npc, corpse in pairs(corpses) do
            if now >= corpse.despawnAt or not exists(npc, "Npc") then removeCorpse(npc) end
        end
    end,
}
