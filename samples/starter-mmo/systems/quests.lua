-- Server system: World of Warcraft-style quests. The definitions are server-only Lua files in .server/quests/ (a
-- dot-folder, so the editor's Build leaves them out of game.pak), read once with kast.definitions and checked here; a
-- bad one refuses the server's start. Each returns:
--   id, title, giver (an npcs/ definition name), text (the offer), progressText, completeText,
--   objectives = {{type = "kill", npc = <npcs/ name>, count, label?} | {type = "collect", item = <items/ id>, count,
--                 label?}, ...} (1..MAX_OBJECTIVES; label defaults to the NPC name + "s slain" / the item's name),
--   rewards = {gold?, items = {{item, count}, ...}?}?, requires = {<quest id>, ...}? (turned in before it is offered).
-- Quests are not repeatable. A character's state is its Quests component (components/quests.lua: active ids, their
-- objectives' progress, completed ids; saved, owner-only).
--
-- Progress: a kill objective counts each death of its NPC for every player who hit it ("npc.died" {definition,
-- damagers}, systems/combat.lua, or "npc.died.away" for those on another map by then), whoever tagged it. A collect objective is the live count of its item in the bag,
-- capped at count: items held before accepting count, and destroying or moving them out lowers it
-- ("inventory.changed" from systems/inventory.lua). No event per objective update: the Quests component carries it.
--
-- Requests (kast.world.send; "client.<name>" {entity, data}, data untrusted). Anything wrong changes nothing and the
-- requester alone gets "quest.rejected" {reason}:
--   "quest.talk"     {npc}         opens the giver's dialog: "quest.dialog" {npc, giver, quests = {{id, title, state
--                                  ("available" | "progress" | "ready"), text (by state), objectives = {{label, count},
--                                  ...}, rewards = {gold, items = {{item, count}, ...}}}, ...}}.
--   "quest.accept"   {npc, quest}  takes the quest: "quest.accepted" {quest, title}, then the dialog again.
--   "quest.complete" {npc, quest}  turns it in: the collect objectives' items leave the bag and the rewards go in, in
--                                  one "inventory.exchange" (systems/inventory.lua; a reward that doesn't fit refuses
--                                  it all: "full", the quest stays ready). "quest.completed" {quest, title, gold,
--                                  items}, then the dialog again.
--   "quest.abandon"  {quest}       drops an active quest and its progress; it can be taken again.
--   "quest.log"      {}            "quest.log" {quests = {...}}: the active quests as in the dialog (text = the offer).
-- Reasons: "dead" (talk, accept, complete, abandon), "unknown_quest", "giver" (npc is not a quest giver, or not this
-- quest's), "range" (giver on another map or beyond TALK_RANGE on x/z), "completed", "active", "requires",
-- "log_full" (MAX_ACTIVE active), "not_active", "incomplete", "full", "busy" (a turn-in of it is under way).
--
-- Markers: for each giver, what the owner's client shows over it: "ready" (a yellow ?: a quest of it can be turned
-- in), else "available" (a yellow !), else "progress" (a grey ?), else "" (nothing). "quest.markers" {markers =
-- {[giver] = state}} goes to the owner on entering and whenever its markers change; nobody else learns them.
--
-- Bandwidth: one answer per request to the requester only, plus the Quests update (owner only) when it changes;
-- "quest.markers" only when a marker changes. Nothing per tick.
local QUEST_FOLDER = ".server/quests"
local MAX_ACTIVE = 20
local MAX_OBJECTIVES = 4 -- per quest: components/quests.lua's progress stride
local TALK_RANGE = 5     -- metres, on x/z (as systems/inventory.lua's LOOT_RANGE)
local MARKER_RANK = {ready = 1, available = 2, progress = 3} -- which state a giver's marker shows first

local QUESTS = {}         -- id -> checked definition
local GIVERS = {}         -- giver name -> its quest ids, in file order
local markersSent = {}    -- player -> the markers last sent, as text
local turningIn = {}      -- player -> {quest, npc} waiting for "inventory.exchanged"

local function exists(e, component)
    local ok, has = pcall(kast.has, e, component) -- raises for ids that are not in the world
    return ok and has
end

local function reject(e, reason)
    kast.sendTo(e, "quest.rejected", {reason = reason})
end

-- Definitions -------------------------------------------------------------------------------------------------------

local function count(v) return type(v) == "number" and v == math.floor(v) and v >= 1 end

-- Checks one definition (path for the message) and fills in the objectives' labels; raises on the first problem.
local function check(q, path)
    local function need(ok, what) if not ok then error(path .. ": " .. what, 0) end end
    need(type(q.id) == "string" and q.id ~= "", "id must be a non-empty string")
    for _, key in ipairs({"title", "text", "progressText", "completeText"}) do
        need(type(q[key]) == "string" and q[key] ~= "", key .. " must be a non-empty string")
    end
    need(type(q.giver) == "string" and kast.npcDef(q.giver) ~= nil, "giver must name an npcs/ definition")
    need(type(q.objectives) == "table" and #q.objectives >= 1 and #q.objectives <= MAX_OBJECTIVES,
         "objectives must list 1 to " .. MAX_OBJECTIVES .. " objectives")
    for i, o in ipairs(q.objectives) do
        local at = "objectives[" .. i .. "]: "
        need(count(o.count), at .. "count must be a whole number of at least 1")
        if o.type == "kill" then
            need(type(o.npc) == "string" and kast.npcDef(o.npc) ~= nil, at .. "npc must name an npcs/ definition")
            o.label = o.label or (o.npc .. "s slain")
        else
            need(o.type == "collect", at .. "type must be \"kill\" or \"collect\"")
            need(kast.items[o.item] ~= nil, at .. "item must name an items/ definition")
            o.label = o.label or kast.items[o.item].name
        end
        need(type(o.label) == "string", at .. "label must be a string")
    end
    q.rewards = q.rewards or {}
    need(q.rewards.gold == nil or (count(q.rewards.gold) or q.rewards.gold == 0), "rewards.gold must be a whole number")
    for i, r in ipairs(q.rewards.items or {}) do
        need(kast.items[r.item] ~= nil and count(r.count), "rewards.items[" .. i .. "] must be {item = <items/ id>, count}")
    end
    q.requires = q.requires or {}
end

local function loadQuests()
    local defs, paths = kast.definitions(QUEST_FOLDER)
    local order = {}
    for i, q in ipairs(defs) do
        check(q, paths[i])
        if QUESTS[q.id] then error(paths[i] .. ": id \"" .. q.id .. "\" is declared twice", 0) end
        QUESTS[q.id] = q
        order[#order + 1] = q.id
    end
    for _, id in ipairs(order) do
        for _, need in ipairs(QUESTS[id].requires) do
            if not QUESTS[need] or need == id then error("quest " .. id .. ": requires unknown quest " .. tostring(need), 0) end
        end
        local giver = QUESTS[id].giver
        GIVERS[giver] = GIVERS[giver] or {}
        table.insert(GIVERS[giver], id)
    end
    kast.log("quests loaded", {count = #order, folder = QUEST_FOLDER})
end

-- State -------------------------------------------------------------------------------------------------------------

-- e's Quests as tables: active = {ids}, progress = {[id] = {n, ...}}, completed = {[id] = true}, done = {ids}.
local function read(e)
    local saved = kast.get(e, "Quests")
    local state = {active = {}, progress = {}, completed = {}, done = {}}
    for i, id in ipairs(saved.active) do
        state.active[i] = id
        local p = {}
        for j = 1, MAX_OBJECTIVES do p[j] = saved.progress[(i - 1) * MAX_OBJECTIVES + j] or 0 end
        state.progress[id] = p
    end
    for _, id in ipairs(saved.completed) do
        state.completed[id] = true
        state.done[#state.done + 1] = id
    end
    return state
end

local function bagCount(e, item)
    local n = 0
    if not exists(e, "Inventory") then return n end
    local inv = kast.get(e, "Inventory")
    for i, id in ipairs(inv.items) do
        if id == item then n = n + (inv.counts[i] or 0) end
    end
    return n
end

-- Sets every active collect objective to the bag's count of its item (capped).
local function countBag(e, state)
    for _, id in ipairs(state.active) do
        for j, o in ipairs(QUESTS[id] and QUESTS[id].objectives or {}) do
            if o.type == "collect" then state.progress[id][j] = math.min(o.count, bagCount(e, o.item)) end
        end
    end
end

local function isComplete(state, q)
    for j, o in ipairs(q.objectives) do
        if state.progress[q.id][j] < o.count then return false end
    end
    return true
end

-- What q is to this character: "ready", "progress", "available" or nil (completed, or its requirements are not).
local function questState(state, q)
    if state.completed[q.id] then return nil end
    if state.progress[q.id] then return isComplete(state, q) and "ready" or "progress" end
    for _, need in ipairs(q.requires) do
        if not state.completed[need] then return nil end
    end
    return "available"
end

local function sendMarkers(e, state)
    local markers, text = {}, {}
    for giver, ids in pairs(GIVERS) do
        local best = ""
        for _, id in ipairs(ids) do
            local s = questState(state, QUESTS[id])
            if s and (best == "" or MARKER_RANK[s] < MARKER_RANK[best]) then best = s end
        end
        markers[giver] = best
        text[#text + 1] = giver .. "=" .. best
    end
    table.sort(text)
    text = table.concat(text, ",")
    if markersSent[e] == text then return end
    markersSent[e] = text
    kast.sendTo(e, "quest.markers", {markers = markers})
end

-- Writes state back (the owner's client gets it) and updates the markers.
local function save(e, state)
    local progress = {}
    for i, id in ipairs(state.active) do
        for j = 1, MAX_OBJECTIVES do progress[(i - 1) * MAX_OBJECTIVES + j] = state.progress[id][j] end
    end
    kast.set(e, "Quests", {active = state.active, progress = progress, completed = state.done})
    sendMarkers(e, state)
end

local function activeIndex(state, id)
    for i, active in ipairs(state.active) do
        if active == id then return i end
    end
end

-- What a client is told about q.
local function describe(q, state, text)
    local objectives = {}
    for j, o in ipairs(q.objectives) do objectives[j] = {label = o.label, count = o.count} end
    return {id = q.id, title = q.title, state = state, text = text, objectives = objectives,
            rewards = {gold = q.rewards.gold or 0, items = q.rewards.items or {}}}
end

local function sendDialog(e, npc, state)
    local def = kast.get(npc, "Npc")
    local list = {}
    for _, id in ipairs(GIVERS[def.npc] or {}) do
        local q = QUESTS[id]
        local s = questState(state, q)
        if s then
            local text = s == "available" and q.text or s == "ready" and q.completeText or q.progressText
            list[#list + 1] = describe(q, s, text)
        end
    end
    table.sort(list, function(a, b) return MARKER_RANK[a.state] < MARKER_RANK[b.state] end)
    kast.sendTo(e, "quest.dialog", {npc = npc, giver = def.name, quests = list})
end

-- Requests ----------------------------------------------------------------------------------------------------------

local function dead(e) return kast.get(e, "Health").current <= 0 end

-- The quest giver data.npc if e may talk to it now (alive, same map, within TALK_RANGE), else rejects and returns nil.
local function giverNear(e, data)
    if dead(e) then return reject(e, "dead") end
    local npc = data.npc
    if type(npc) ~= "number" or not exists(npc, "Npc") or not GIVERS[kast.get(npc, "Npc").npc] then
        return reject(e, "giver")
    end
    local from, to = kast.get(e, "Transform"), kast.get(npc, "Transform")
    local dx, dz = from.position.x - to.position.x, from.position.z - to.position.z
    if from.map ~= to.map or math.sqrt(dx * dx + dz * dz) > TALK_RANGE then return reject(e, "range") end
    return npc
end

-- The quest data.quest of data.npc, if e may deal with it there now; else rejects and returns nil.
local function questAtGiver(e, data)
    if dead(e) then return reject(e, "dead") end
    local q = QUESTS[data.quest]
    if not q then return reject(e, "unknown_quest") end
    local npc = giverNear(e, data)
    if not npc then return end
    if kast.get(npc, "Npc").npc ~= q.giver then return reject(e, "giver") end
    return q, npc
end

local function request(handler)
    return function(event)
        local data = type(event.data) == "table" and event.data or {}
        handler(event.entity, data)
    end
end

local function onTalk(e, data)
    local npc = giverNear(e, data)
    if npc then sendDialog(e, npc, read(e)) end
end

local function onAccept(e, data)
    local q, npc = questAtGiver(e, data)
    if not q then return end
    local state = read(e)
    if state.completed[q.id] then return reject(e, "completed") end
    if state.progress[q.id] then return reject(e, "active") end
    if questState(state, q) ~= "available" then return reject(e, "requires") end
    if #state.active >= MAX_ACTIVE then return reject(e, "log_full") end
    state.active[#state.active + 1] = q.id
    state.progress[q.id] = {0, 0, 0, 0}
    countBag(e, state)
    save(e, state)
    kast.sendTo(e, "quest.accepted", {quest = q.id, title = q.title})
    sendDialog(e, npc, state)
end

local function onComplete(e, data)
    local q, npc = questAtGiver(e, data)
    if not q then return end
    local state = read(e)
    if not state.progress[q.id] then return reject(e, "not_active") end
    if turningIn[e] then return reject(e, "busy") end
    countBag(e, state)
    if not isComplete(state, q) then return reject(e, "incomplete") end
    local take = {}
    for _, o in ipairs(q.objectives) do
        if o.type == "collect" then take[#take + 1] = {item = o.item, count = o.count} end
    end
    turningIn[e] = {quest = q.id, npc = npc}
    kast.publish("inventory.exchange", {entity = e, ref = q.id, take = take, give = q.rewards.items or {},
                                        gold = q.rewards.gold or 0})
end

local function onExchanged(event)
    local e = event.entity
    local turn = turningIn[e]
    if not turn or turn.quest ~= event.ref then return end -- someone else's exchange
    turningIn[e] = nil
    if not exists(e, "Quests") then return end
    if not event.ok then return reject(e, event.reason == "full" and "full" or "incomplete") end
    local q, state = QUESTS[turn.quest], read(e)
    local i = activeIndex(state, q.id)
    if not i then return end
    table.remove(state.active, i)
    state.progress[q.id] = nil
    state.completed[q.id] = true
    state.done[#state.done + 1] = q.id
    countBag(e, state)
    save(e, state)
    kast.sendTo(e, "quest.completed", {quest = q.id, title = q.title, gold = q.rewards.gold or 0,
                                       items = q.rewards.items or {}})
    if exists(turn.npc, "Npc") then sendDialog(e, turn.npc, state) end
end

local function onAbandon(e, data)
    if dead(e) then return reject(e, "dead") end
    local state = read(e)
    local i = activeIndex(state, data.quest)
    if not i then return reject(e, "not_active") end
    if turningIn[e] and turningIn[e].quest == data.quest then return reject(e, "busy") end
    table.remove(state.active, i)
    state.progress[data.quest] = nil
    save(e, state)
    kast.sendTo(e, "quest.abandoned", {quest = data.quest, title = QUESTS[data.quest] and QUESTS[data.quest].title})
end

local function onLog(e)
    local state, list = read(e), {}
    for _, id in ipairs(state.active) do
        local q = QUESTS[id]
        if q then list[#list + 1] = describe(q, questState(state, q), q.text) end
    end
    kast.sendTo(e, "quest.log", {quests = list})
end

-- Every player who hit the dead NPC and has a kill objective for it gets one more.
local function onDied(event)
    for _, player in ipairs(event.damagers or {}) do
        if exists(player, "Quests") then
            local state, changed = read(player), false
            for _, id in ipairs(state.active) do
                for j, o in ipairs(QUESTS[id] and QUESTS[id].objectives or {}) do
                    if o.type == "kill" and o.npc == event.definition and state.progress[id][j] < o.count then
                        state.progress[id][j] = state.progress[id][j] + 1
                        changed = true
                    end
                end
            end
            if changed then save(player, state) end
        end
    end
end

local function onBagChanged(event)
    local e = event.entity
    if not exists(e, "Quests") or turningIn[e] then return end -- a turn-in's own take: onExchanged counts the bag
    local state = read(e)
    local before = {}
    for _, id in ipairs(state.active) do before[id] = table.concat(state.progress[id], ",") end
    countBag(e, state)
    for _, id in ipairs(state.active) do
        if before[id] ~= table.concat(state.progress[id], ",") then return save(e, state) end
    end
    sendMarkers(e, state) -- only if they changed
end

return {
    init = function()
        loadQuests()
        kast.subscribe("client.quest.talk", request(onTalk))
        kast.subscribe("client.quest.accept", request(onAccept))
        kast.subscribe("client.quest.complete", request(onComplete))
        kast.subscribe("client.quest.abandon", request(onAbandon))
        kast.subscribe("client.quest.log", request(onLog))
        kast.subscribe("inventory.exchanged", onExchanged)
        kast.subscribe("inventory.changed", onBagChanged)
        kast.subscribe("npc.died", onDied)
        kast.subscribe("npc.died.away", function(p) onDied(p.data) end) -- damagers now on this map (kast.world.post)
    end,

    -- A saved state from an older .server/quests/ set: unknown, repeated and surplus quests dropped, kill progress
    -- capped; collect progress is the bag's.
    onEnter = function(e)
        if not kast.has(e, "Quests") then kast.add(e, "Quests") end
        local saved, state = read(e), {active = {}, progress = {}, completed = {}, done = {}}
        for _, id in ipairs(saved.done) do
            if QUESTS[id] and not state.completed[id] then
                state.completed[id] = true
                state.done[#state.done + 1] = id
            end
        end
        for _, id in ipairs(saved.active) do
            local q = QUESTS[id]
            if q and not state.progress[id] and not state.completed[id] and #state.active < MAX_ACTIVE then
                state.active[#state.active + 1] = id
                state.progress[id] = {0, 0, 0, 0}
                for j, o in ipairs(q.objectives) do state.progress[id][j] = math.min(o.count, saved.progress[id][j]) end
            end
        end
        countBag(e, state)
        markersSent[e] = nil
        save(e, state)
    end,

    onLeave = function(e)
        markersSent[e], turningIn[e] = nil, nil
    end,

    -- Each map has its own Lua state (docs/services/world/maps.html#map-contexts): the markers last sent go with the
    -- character. A turn-in under way is dropped (its "inventory.exchanged" comes on this map's bus): the quest stays
    -- ready to turn in again.
    onMapLeave = function(e)
        local sent = markersSent[e]
        markersSent[e], turningIn[e] = nil, nil
        return {markers = sent}
    end,
    onMapEnter = function(e, carried) markersSent[e] = carried and carried.markers end,

    update = function(dt) end,
}
