-- TEST ONLY (ctest starter_mmo_lists, cmake/starter_mmo_lists.cmake): added to a copy of samples/starter-mmo's
-- meadow.scene, never to the sample itself. Offline, so it plays the server's part: 15 chat lines (scripts/chat.lua's
-- "chat.local"), 25 active quests (the replicated Quests and the server's quest.log, as the world client publishes
-- them) and the quest log opened; then whether rows 24 and 25 exist (0-based: 25 rows) and the 23rd row selected, and
-- LISTSHOT (for --screenshot-on-log) once both have drawn.
local CHAT_LINES, QUESTS = 15, 25
local frame = 0

return {
    update = function(dt)
        frame = frame + 1
        if frame == 30 then
            for i = 1, CHAT_LINES do kast.publish("chat.local", {text = "Chat line " .. i}) end
            local active, progress, quests = {}, {}, {}
            for i = 1, QUESTS do
                local id = "test_quest_" .. i
                active[i] = id
                for j = 1, 4 do progress[(i - 1) * 4 + j] = j == 1 and i % 4 or 0 end
                quests[i] = {id = id, title = "Quest " .. i, text = "The text of quest " .. i .. ".",
                             objectives = {{label = "Bats slain", count = 3}},
                             rewards = {gold = i, items = {}}}
            end
            kast.publish("world.component", {name = "Quests", data = {active = active, progress = progress, completed = {}}})
            kast.publish("quest_log.toggle", {})
            kast.publish("server.quest.log", {quests = quests})
        elseif frame == 35 then
            -- rows past the old 20 slots exist (0-based ids) and select: the detail shows Quest 23
            kast.log(string.format("test lists: row 24 %s, row 25 %s, chat lines %d", tostring(kast.gui.focus("qlog_row_24")),
                                   tostring(kast.gui.focus("qlog_row_25")), #kast.gui.get("chat", "lines")))
            kast.publish("quest.log_select", {id = "qlog_row_22"})
        elseif frame == 40 then
            kast.log("test lists: LISTSHOT")
        end
    end,
}
