-- The quest log (ui/quest_log.rml), client side. The server (systems/quests.lua) owns the quests:
--   L (or the "quest_log.toggle" event) opens and closes it; not while typing in a GUI text field. Opening it asks
--   kast.world.send("quest.log", {}) once; the server's "quest.log" {quests} gives each active quest's title, text,
--   objectives and rewards. The rows (model "quest_log", key "rows" in ui/quest_log.rml, one per quest) show each active
--   quest of the replicated Quests (components/quests.lua) with its objectives' progress ("Bats slain: 2/3"), updated as
--   Quests changes; a change of which quests are active asks again, once. Clicking a row (qlog_row_<0-based index>)
--   shows its text and rewards; Abandon sends "quest.abandon" {quest}.
-- Opening or closing publishes "quest_log.visibility" {open}. "quest.abandoned" becomes a chat box line.
--
-- Bandwidth: one ClientEvent when it opens, per Abandon, and when the active quests change while it is open.
-- The server's cap on active quests (systems/quests.lua), shown in the title; components/quests.lua's stride; the reward
-- slots of ui/quest_log.rml.
local LOG_SIZE, MAX_OBJECTIVES, MAX_REWARDS = 20, 4, 3

local me
local open = false
local held = false     -- L down last frame
local mine = {active = {}, progress = {}} -- the replicated Quests
local info = {}        -- quest id -> the server's description (quest.log)
local askedFor         -- the active quests (joined) when quest.log was last asked for
local selected         -- quest id shown below the rows
local rowIds = {}      -- quest id of each row, as last drawn

local function log(text) kast.publish("chat.local", {text = text}) end

local function ask()
    askedFor = table.concat(mine.active, ",")
    kast.world.send("quest.log", {})
end

local function drawRewards(rewards)
    kast.gui.setVisible("qlog_reward_gold", rewards.gold > 0)
    kast.gui.setText("qlog_reward_gold_name", rewards.gold .. " Gold")
    for row = 1, MAX_REWARDS do
        local entry = rewards.items[row]
        local def = entry and kast.items[entry.item]
        kast.gui.setVisible("qlog_reward_" .. row, entry ~= nil)
        if entry then
            local name = def and def.name or entry.item
            kast.gui.setText("qlog_reward_" .. row .. "_name", entry.count > 1 and (name .. " x" .. entry.count) or name)
            kast.gui.setAttribute("qlog_reward_" .. row .. "_name", "class", "loot_name q_" .. (def and def.quality or "common"))
            kast.gui.setAttribute("qlog_reward_" .. row .. "_icon", "style",
                                  def and ("decorator: image(../" .. def.icon .. ")") or "decorator: none")
        end
    end
    kast.gui.setVisible("qlog_rewards_heading", rewards.gold > 0 or #rewards.items > 0)
end

local function draw()
    local shown = {}
    for i, id in ipairs(mine.active) do
        if info[id] then shown[#shown + 1] = {id = id, index = i} end
    end
    if not selected or not info[selected] then selected = shown[1] and shown[1].id end
    kast.gui.setText("qlog_count", string.format("(%d/%d)", #mine.active, LOG_SIZE))
    kast.gui.setVisible("qlog_empty", #mine.active == 0)
    local rows = {}
    rowIds = {}
    for row, s in ipairs(shown) do
        local q = info[s.id]
        local lines = {}
        for j, o in ipairs(q.objectives) do
            local n = mine.progress[(s.index - 1) * MAX_OBJECTIVES + j] or 0
            lines[#lines + 1] = string.format("%s: %d/%d", o.label, n, o.count)
        end
        rows[row] = {title = q.title, progress = table.concat(lines, ", "), selected = s.id == selected}
        rowIds[row] = s.id
    end
    kast.gui.set("quest_log", "rows", rows)
    kast.gui.setVisible("qlog_detail", selected ~= nil)
    if not selected then return end
    kast.gui.setText("qlog_title", info[selected].title)
    kast.gui.setText("qlog_text", info[selected].text)
    drawRewards(info[selected].rewards)
end

-- Draws; asks the server again first when the active quests changed since it was last asked.
local function refresh()
    if table.concat(mine.active, ",") ~= askedFor then ask() end
    draw()
end

local function setOpen(value)
    if value ~= open then kast.publish("quest_log.visibility", {open = value}) end
    open = value
    kast.gui.setVisible("quest_log", open)
    if open then
        info = {} -- descriptions may have changed since the last look
        ask()
        draw()
    end
end

return {
    init = function()
        setOpen(false)
        kast.subscribe("world.entered", function(e) me = e.entityId end)
        kast.subscribe("world.component", function(e)
            if e.entityId ~= me or e.name ~= "Quests" then return end
            mine = e.data
            if open then refresh() end
        end)
        kast.subscribe("server.quest.log", function(e)
            info = {}
            for _, q in ipairs(e.quests or {}) do info[q.id] = q end
            if open then draw() end
        end)
        kast.subscribe("quest_log.toggle", function() setOpen(not open) end)
        kast.subscribe("quest.log_select", function(e)
            local row = tonumber(string.match(e.id or "", "^qlog_row_(%d+)$"))
            selected = row and rowIds[row + 1] or selected
            draw()
        end)
        kast.subscribe("quest.log_abandon", function()
            if open and selected then kast.world.send("quest.abandon", {quest = selected}) end
        end)
        kast.subscribe("server.quest.abandoned", function(e) log("Quest abandoned: " .. (e.title or e.quest)) end)
        kast.subscribe("world.disconnected", function() setOpen(false) end)
    end,

    update = function(dt)
        local inWorld = kast.world and kast.world.state() == "inWorld"
        local down = inWorld and not kast.gui.focusedField() and kast.input.keyDown("L")
        if down and not held then setOpen(not open) end
        held = down
    end,
}
