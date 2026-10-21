-- Quest givers and their dialog (ui/quest.rml), client side. The server (systems/quests.lua) owns the quests and their
-- definitions; this only asks and shows what it answers:
--   Markers: the server's "quest.markers" {markers = {[giver name] = "available" | "ready" | "progress" | ""}} (sent
--   to this player alone, kept across zone scenes with replay) puts a yellow ! (available), a yellow ? (ready to turn
--   in) or a grey ? (in progress) over each clone of that NPC, built from cube primitives (GLYPHS, MARKER_*) and turned
--   to face the camera every frame.
--   A right click on a friendly NPC (scripts/combat.lua's "npc.clicked" {entityId, name}) that is a quest giver: beyond
--   TALK_RANGE says "Too far away"; else kast.world.send("quest.talk", {npc}), and the server's "quest.dialog" {npc,
--   giver, quests} opens the dialog: its quests (up to MAX_CHOICES), the chosen one's text, objectives (with the
--   replicated Quests progress once taken), rewards (gold, item icons with quality-coloured names), and Accept
--   ("quest.accept" {npc, quest}) or Complete ("quest.complete" {npc, quest}). The server answers with the dialog
--   again; one with nothing left to show closes it.
--   The dialog closes on Close, Esc (not while typing in a GUI text field), more than TALK_RANGE away, the giver's
--   despawn, a map change or a disconnect.
--   "quest.rejected" {reason}, "quest.accepted" and "quest.completed" become chat box lines ("chat.local").
--
-- Bandwidth: one ClientEvent per click; nothing per frame (the markers turn locally).
local TALK_RANGE = 5 -- m on x/z, as the server checks
local MAX_CHOICES, MAX_OBJECTIVES, MAX_REWARDS = 4, 4, 3 -- rows in ui/quest.rml
local MARKER_HEIGHT = 2.6 -- m above the giver's feet
local MARKER_CELL = 0.11  -- m per glyph cell
local MARKER_MATERIAL = {available = "materials/quest_yellow.mat", ready = "materials/quest_yellow.mat",
                         progress = "materials/quest_grey.mat"}
local GLYPHS = { -- boxes {x0, y0, x1, y1} in cells, facing +z
    ["!"] = {{-0.5, 2, 0.5, 7}, {-0.5, 0, 0.5, 1}},
    ["?"] = {{-1.5, 6, 1.5, 7}, {-2.5, 5, -1.5, 6}, {1.5, 4, 2.5, 6}, {-0.5, 3, 1.5, 4}, {-0.5, 2, 0.5, 3},
             {-0.5, 0, 0.5, 1}},
}
local GLYPH_OF = {available = "!", ready = "?", progress = "?"}
local REJECTED = {
    dead = "You are dead",
    unknown_quest = "Unknown quest",
    giver = "Invalid target",
    range = "Too far away",
    completed = "You have already completed that quest",
    active = "You are already on that quest",
    requires = "You can't take that quest yet",
    log_full = "Quest log is full",
    not_active = "You are not on that quest",
    incomplete = "Quest not complete",
    full = "Inventory is full",
    busy = "Not ready yet",
}

local me, player
local npcs = {}        -- NPC server id -> {entity = clone, name =}
local states = {}      -- giver name -> its marker state for this player ("" none)
local markers = {}     -- NPC server id -> {entity =, state =}
local mine = {active = {}, progress = {}} -- the replicated Quests
local open             -- server id of the giver whose dialog shows
local offered = {}     -- the open dialog's quests
local chosen           -- index into offered
local escHeld = false

local function log(text) kast.publish("chat.local", {text = text}) end

local function positionOf(id)
    local ok, tf = pcall(kast.get, npcs[id] and npcs[id].entity, "Transform")
    return ok and tf.position or nil
end

local function flatDistance(id)
    local a, b = kast.get(player, "Transform").position, positionOf(id)
    return b and math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2) or math.huge
end

-- Markers -----------------------------------------------------------------------------------------------------------

local function dropMarker(id)
    if markers[id] then pcall(kast.destroy, markers[id].entity) end
    markers[id] = nil
end

local function buildMarker(state)
    local root = kast.create("Quest Marker")
    for i, box in ipairs(GLYPHS[GLYPH_OF[state]]) do
        local part = kast.create("Quest Marker " .. i, root)
        kast.set(part, "Transform", {
            position = {x = (box[1] + box[3]) / 2 * MARKER_CELL, y = (box[2] + box[4]) / 2 * MARKER_CELL, z = 0},
            scale = {x = (box[3] - box[1]) * MARKER_CELL, y = (box[4] - box[2]) * MARKER_CELL, z = MARKER_CELL},
        })
        kast.add(part, "MeshRenderer")
        kast.set(part, "MeshRenderer", {primitive = "cube", material = MARKER_MATERIAL[state], castShadows = false})
    end
    return root
end

-- The marker over NPC id as its giver's state says: built, rebuilt or removed.
local function refreshMarker(id)
    local state = npcs[id] and states[npcs[id].name] or ""
    if markers[id] and markers[id].state == state then return end
    dropMarker(id)
    if state ~= "" and GLYPH_OF[state] then markers[id] = {entity = buildMarker(state), state = state} end
end

local function cameraPosition()
    for e in kast.query("Camera") do return kast.get(e, "Transform").position end
end

-- Dialog ------------------------------------------------------------------------------------------------------------

local function close()
    open, offered, chosen = nil, {}, nil
    kast.gui.setVisible("quest_dialog", false)
end

-- The replicated progress of objective j of an active quest id, or nil if it isn't active.
local function progressOf(id, j)
    for i, active in ipairs(mine.active) do
        if active == id then return mine.progress[(i - 1) * MAX_OBJECTIVES + j] or 0 end
    end
end

local function drawRewards(prefix, rewards)
    kast.gui.setVisible(prefix .. "_reward_gold", rewards.gold > 0)
    kast.gui.setText(prefix .. "_reward_gold_name", rewards.gold .. " Gold")
    for row = 1, MAX_REWARDS do
        local entry = rewards.items[row]
        local def = entry and kast.items[entry.item]
        kast.gui.setVisible(prefix .. "_reward_" .. row, entry ~= nil)
        if entry then
            local name = def and def.name or entry.item
            kast.gui.setText(prefix .. "_reward_" .. row .. "_name", entry.count > 1 and (name .. " x" .. entry.count) or name)
            kast.gui.setAttribute(prefix .. "_reward_" .. row .. "_name", "class", "loot_name q_" .. (def and def.quality or "common"))
            kast.gui.setAttribute(prefix .. "_reward_" .. row .. "_icon", "style",
                                  def and ("decorator: image(../" .. def.icon .. ")") or "decorator: none")
        end
    end
    kast.gui.setVisible(prefix .. "_rewards_heading", rewards.gold > 0 or #rewards.items > 0)
end

local function draw()
    for row = 1, MAX_CHOICES do
        local q = offered[row]
        kast.gui.setVisible("quest_choice_" .. row, q ~= nil)
        if q then
            kast.gui.setText("quest_choice_" .. row .. "_mark", GLYPH_OF[q.state])
            kast.gui.setAttribute("quest_choice_" .. row .. "_mark", "class", "quest_mark " .. q.state)
            kast.gui.setText("quest_choice_" .. row .. "_title", q.title)
            kast.gui.setAttribute("quest_choice_" .. row, "class", row == chosen and "quest_choice selected" or "quest_choice")
        end
    end
    local q = offered[chosen]
    kast.gui.setText("quest_title", q.title)
    kast.gui.setText("quest_text", q.text)
    for j = 1, MAX_OBJECTIVES do
        local o = q.objectives[j]
        local n = o and progressOf(q.id, j)
        kast.gui.setVisible("quest_objective_" .. j, o ~= nil)
        if o then kast.gui.setText("quest_objective_" .. j, n and string.format("%s: %d/%d", o.label, n, o.count)
                                                             or string.format("%s: %d", o.label, o.count)) end
    end
    drawRewards("quest", q.rewards)
    kast.gui.setVisible("quest_accept", q.state == "available")
    kast.gui.setVisible("quest_complete", q.state == "ready")
end

local function onDialog(e)
    local wasOpen = open == e.npc
    local before = offered[chosen] and offered[chosen].id
    if #(e.quests or {}) == 0 then
        close()
        if not wasOpen then log("* " .. e.giver .. " has nothing for you right now.") end
        return
    end
    open, offered, chosen = e.npc, e.quests, 1
    for i, q in ipairs(offered) do
        if q.id == before then chosen = i end
    end
    kast.gui.setText("quest_giver", e.giver)
    kast.gui.setVisible("quest_dialog", true)
    draw()
end

local function onClicked(e)
    if states[e.name] == nil then return end -- not a quest giver
    if flatDistance(e.entityId) > TALK_RANGE then return log("* Too far away") end
    kast.world.send("quest.talk", {npc = e.entityId})
end

local function forget(id)
    if id == open then close() end
    dropMarker(id)
    npcs[id] = nil
end

local function forgetAll()
    for id in pairs(npcs) do forget(id) end
    close()
end

return {
    init = function()
        close()
        for e in kast.query("NetworkLocalPlayer") do player = e end
        kast.subscribe("world.entered", function(e) me = e.entityId end)
        kast.subscribe("world.entitySpawned", function(e)
            if e.kind == "npc" then npcs[e.entityId] = {entity = e.entity, name = e.name} end
        end)
        kast.subscribe("world.despawn", function(e) forget(e.entityId) end)
        kast.subscribe("world.mapChanged", forgetAll)
        kast.subscribe("world.disconnected", forgetAll)
        kast.subscribe("world.component", function(e)
            if e.entityId ~= me or e.name ~= "Quests" then return end
            mine = e.data
            if open then draw() end
        end)
        -- replay: sent on entering and on changes, maybe before this zone's scene loaded
        kast.subscribe("server.quest.markers", function(e)
            states = e.markers
        end, {replay = true})
        kast.subscribe("npc.clicked", onClicked)
        kast.subscribe("server.quest.dialog", onDialog)
        kast.subscribe("quest.choose", function(e)
            local row = tonumber(string.match(e.id or "", "^quest_choice_(%d+)$"))
            if open and row and offered[row] then
                chosen = row
                draw()
            end
        end)
        kast.subscribe("quest.accept", function()
            if open then kast.world.send("quest.accept", {npc = open, quest = offered[chosen].id}) end
        end)
        kast.subscribe("quest.complete", function()
            if open then kast.world.send("quest.complete", {npc = open, quest = offered[chosen].id}) end
        end)
        kast.subscribe("quest.close", close)
        kast.subscribe("server.quest.rejected", function(e) log("* " .. (REJECTED[e.reason] or e.reason)) end)
        kast.subscribe("server.quest.accepted", function(e) log("Quest accepted: " .. e.title) end)
        kast.subscribe("server.quest.completed", function(e) log("Quest completed: " .. e.title) end)
    end,

    update = function(dt)
        for id in pairs(npcs) do refreshMarker(id) end -- here, not in the events: no entities made mid-dispatch
        local camera = cameraPosition()
        for id, m in pairs(markers) do
            local p = positionOf(id)
            if p then
                local yaw = camera and math.atan2(camera.x - p.x, camera.z - p.z) or 0 -- +z toward the camera
                kast.set(m.entity, "Transform", {position = {x = p.x, y = p.y + MARKER_HEIGHT, z = p.z},
                                                 rotation = {x = 0, y = math.sin(yaw / 2), z = 0, w = math.cos(yaw / 2)}})
            end
        end
        local esc = not kast.gui.focusedField() and kast.input.keyDown("Escape")
        if open and ((esc and not escHeld) or flatDistance(open) > TALK_RANGE) then close() end
        escHeld = esc
    end,
}
