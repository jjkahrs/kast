-- Looting, client side. The server (systems/inventory.lua) rolls each NPC's loot when it dies and gives it to the player
-- who hit it first; the corpse's replicated Corpse {owner} (components/corpse.lua) says whose it is, never what it holds.
--   A sparkle (SPARKLE_*) bobs over each corpse this player owns, until it goes (emptied or after 60 s).
--   A right click on a corpse (scripts/combat.lua's "loot.clicked" {entityId}): someone else's says so in the chat box;
--   beyond LOOT_RANGE says "Too far away"; else kast.world.send("loot.open", {target}), and the server's
--   "loot.contents" {target, gold, items} opens the loot window (ui/loot.rml).
--   A click on an entry takes it ("loot.take" {target, index}, index "gold" or 1..n), Take All takes everything
--   ("loot.take_all" {target}). The server answers with "loot.contents" again (what is left; the bag shows the rest)
--   or "loot.closed" when the corpse is empty and goes.
--   The window closes on "loot.closed", when the corpse despawns, when the player is more than LOOT_RANGE away, on Esc
--   (not while typing in a GUI text field) and on a map change.
--
-- Bandwidth: one ClientEvent per click; nothing per frame.
local LOOT_RANGE = 5 -- m on x/z, as the server checks
local MAX_ENTRIES = 4 -- item rows in ui/loot.rml
local SPARKLE_MATERIAL = "materials/gold.mat"
local SPARKLE_SCALE = {x = 0.3, y = 0.3, z = 0.3}
local SPARKLE_HEIGHT, SPARKLE_BOB, SPARKLE_RATE = 1.6, 0.25, 3 -- m above the corpse, m up and down, radians/s

local me, player
local clones = {}    -- NPC server id -> clone entity
local owners = {}    -- NPC server id -> Corpse.owner
local sparkles = {}  -- NPC server id -> sparkle entity
local open           -- server id of the corpse whose loot window shows
local entries = {}   -- row -> index into its items (what loot.take wants)
local time = 0
local escHeld = false

local function log(text) kast.publish("chat.local", {text = text}) end

-- Where corpse id's clone is; nil if it is gone (replication removes clones on despawn).
local function corpseAt(id)
    local ok, tf = pcall(kast.get, clones[id], "Transform")
    return ok and tf.position or nil
end

local function flatDistance(id)
    local a, b = kast.get(player, "Transform").position, corpseAt(id)
    return b and math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2) or math.huge
end

-- Shows a sparkle over the corpse id if this player owns it and its clone exists, removes it otherwise.
local function refreshSparkle(id)
    local want = clones[id] ~= nil and me ~= nil and owners[id] == me
    if want and not sparkles[id] then
        local s = kast.create("Loot Sparkle") -- not the clone's child: the clone lies on its side
        kast.set(s, "Transform", {scale = SPARKLE_SCALE})
        kast.add(s, "MeshRenderer")
        kast.set(s, "MeshRenderer", {primitive = "sphere", material = SPARKLE_MATERIAL, castShadows = false})
        sparkles[id] = s
    elseif not want and sparkles[id] then
        kast.destroy(sparkles[id])
        sparkles[id] = nil
    end
end

local function close()
    open = nil
    kast.gui.setVisible("loot", false)
end

local function show(e)
    open = e.target
    kast.gui.setVisible("loot", true)
    kast.gui.setVisible("loot_gold", e.gold > 0)
    kast.gui.setText("loot_gold_name", e.gold .. " Gold")
    entries = {}
    for i, entry in ipairs(e.items or {}) do
        if i <= MAX_ENTRIES then entries[i] = i end
    end
    for row = 1, MAX_ENTRIES do
        local entry = e.items[row]
        local def = entry and kast.items[entry.item]
        kast.gui.setVisible("loot_entry_" .. row, entry ~= nil)
        if entry then
            local name = def and def.name or entry.item
            kast.gui.setText("loot_entry_" .. row .. "_name", entry.count > 1 and (name .. " x" .. entry.count) or name)
            kast.gui.setAttribute("loot_entry_" .. row .. "_name", "class", "loot_name q_" .. (def and def.quality or "common"))
            kast.gui.setAttribute("loot_entry_" .. row .. "_icon", "style", def and ("decorator: image(../" .. def.icon .. ")") or "")
        end
    end
end

local function forget(id)
    if id == open then close() end
    clones[id], owners[id] = nil, nil
    refreshSparkle(id)
end

local function forgetAll()
    for id in pairs(clones) do forget(id) end
    close()
end

local function onClicked(e)
    local id = e.entityId
    if owners[id] ~= me then return log("* You can't loot that") end
    if flatDistance(id) > LOOT_RANGE then return log("* Too far away") end
    kast.world.send("loot.open", {target = id})
end

return {
    init = function()
        close()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        kast.subscribe("world.entered", function(e) me = e.entityId end)
        kast.subscribe("world.entitySpawned", function(e)
            clones[e.entityId] = e.entity
            refreshSparkle(e.entityId)
        end)
        kast.subscribe("world.component", function(e)
            if e.name ~= "Corpse" then return end
            owners[e.entityId] = e.data.owner
            refreshSparkle(e.entityId)
        end)
        kast.subscribe("world.despawn", function(e) forget(e.entityId) end)
        kast.subscribe("world.mapChanged", forgetAll)
        kast.subscribe("world.disconnected", forgetAll)
        kast.subscribe("loot.clicked", onClicked)
        kast.subscribe("server.loot.contents", show)
        kast.subscribe("server.loot.closed", function(e) if e.target == open then close() end end)
        kast.subscribe("loot.take_gold", function()
            if open then kast.world.send("loot.take", {target = open, index = "gold"}) end
        end)
        kast.subscribe("loot.take_entry", function(e)
            local row = tonumber(string.match(e.id, "^loot_entry_(%d+)$"))
            if open and row and entries[row] then kast.world.send("loot.take", {target = open, index = entries[row]}) end
        end)
        kast.subscribe("loot.take_all", function()
            if open then kast.world.send("loot.take_all", {target = open}) end
        end)
    end,

    update = function(dt)
        time = time + dt
        local bob = SPARKLE_HEIGHT + SPARKLE_BOB * math.sin(time * SPARKLE_RATE)
        for id, s in pairs(sparkles) do
            local p = corpseAt(id)
            if p then kast.set(s, "Transform", {position = {x = p.x, y = p.y + bob, z = p.z}}) end
        end
        local esc = not kast.gui.focusedField() and kast.input.keyDown("Escape")
        if open and ((esc and not escHeld) or flatDistance(open) > LOOT_RANGE) then close() end
        escHeld = esc
    end,
}
