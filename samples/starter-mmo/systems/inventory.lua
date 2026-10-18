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
-- Bandwidth: one event per request back to the requester only, plus the Inventory / Wallet change (owner only) and the
-- Corpse component once per death (everyone in view). Nothing per tick.
local SLOTS = 16
local LOOT_RANGE = 5        -- metres, on x/z
local CORPSE_SECONDS = 60   -- a corpse with loot left goes after this
-- ponytail: math.random's sequence starts the same on every server start (the sandbox has no clock to seed it from);
-- fine for a sample, seed it from a real source in a game where drops matter.

local now = 0
local corpses = {} -- npc -> {owner = player, gold = n, items = {{item =, count =}, ...}, despawnAt = time}

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
        kast.subscribe("npc.died", onDied)
        kast.subscribe("client.loot.open", onLootOpen)
        kast.subscribe("client.loot.take", onLootTake)
        kast.subscribe("client.loot.take_all", onLootTakeAll)
    end,

    onEnter = function(e)
        if not kast.has(e, "Inventory") then kast.add(e, "Inventory") end
        writeBag(e, readBag(e)) -- a saved bag from an older items/ set: unknown items dropped, always SLOTS slots
    end,

    update = function(dt)
        now = now + dt
        for npc, corpse in pairs(corpses) do
            if now >= corpse.despawnAt or not exists(npc, "Npc") then removeCorpse(npc) end
        end
    end,
}
