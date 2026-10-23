-- Drives ui/lists.rml (ctest ui_lists_*, player_lists_*): list selection events, kast.gui.value / setValue / scrollTo /
-- scrollToEnd, and data-follow while rows are appended.
-- init: every list event is logged with its id, value and the value's Lua type; model "lists" gets 3 lines and 100,000
-- items {id = n, name = "Item n"}. Frame 2: the values read back (VALUES). Frame 3: setValue by value, key and index,
-- refused values (logged, never the value) and wrong argument types that raise (SETVALUE). Frame 4: scrollTo the middle
-- of the Virtual List and to the last authored row, an out-of-range row and an unknown id (both logged: SCROLLTO).
-- Frame 5: the Virtual List takes focus and KEYS is logged (--sim-on-log: keys follow). Every 15 frames a line is
-- appended (the data-for list follows it). Frame 150: scrollToEnd on the Virtual List, then at frames 160, 180 and 200
-- an item, which it follows (APPEND); at frame 220 the list is scrolled to the middle again and the append at 240 leaves
-- it there (APPEND ... kept). Each append sets the whole 100,000-row array again (kast.gui.set converts it).
local frame = 0
local lines = {}
local items = {}

local function show(value)
    return string.format("%s(%s)", tostring(value), type(value))
end

local function addLine()
    local n = #lines + 1
    lines[n] = {id = n, text = string.format("Line %d", n)}
    kast.gui.set("lists", "lines", lines)
end

local function addItem()
    local n = #items + 1
    items[n] = {id = n, name = string.format("Item %d", n)}
    kast.gui.set("lists", "items", items)
    kast.gui.set("lists", "total", n)
    kast.log(string.format("lists: APPEND items=%d", n))
end

return {
    init = function()
        for _, name in ipairs({"classes", "log", "big"}) do
            kast.subscribe("lists." .. name, function(e)
                kast.log(string.format("lists: lists.%s %s = %s in %s", name, e.id, show(e.value), e.document))
            end)
        end
        for _ = 1, 3 do
            addLine()
        end
        for n = 1, 100000 do
            items[n] = {id = n, name = string.format("Item %d", n)}
        end
        kast.gui.set("lists", "items", items)
        kast.gui.set("lists", "total", #items)
    end,
    update = function()
        frame = frame + 1
        if frame == 2 then
            kast.log(string.format("lists: VALUES classes=%s log=%s big=%s", show(kast.gui.value("classes")),
                                   show(kast.gui.value("log")), show(kast.gui.value("big"))))
        elseif frame == 3 then
            kast.gui.setValue("classes", "rogue")
            kast.gui.setValue("log", 1)
            kast.gui.setValue("big", "50000")
            local byKey = kast.gui.value("big")
            kast.gui.setValue("big", 49999) -- by index: the same row
            local byIndex = kast.gui.value("big")
            kast.gui.setValue("classes", "paladin") -- no such row: logged, unchanged
            kast.gui.setValue("big", 100000) -- past the end: logged, unchanged
            local raised = {
                not pcall(function() kast.gui.setValue("big", {}) end),
                not pcall(function() kast.gui.scrollTo("big", "middle") end),
                not pcall(function() kast.gui.scrollTo("big", 1.5) end),
                not pcall(function() kast.gui.scrollToEnd(7) end),
            }
            kast.log(string.format("lists: SETVALUE classes=%s log=%s big=%s %s raised=%s %s %s %s",
                                   show(kast.gui.value("classes")), show(kast.gui.value("log")), show(byKey),
                                   show(byIndex), tostring(raised[1]), tostring(raised[2]), tostring(raised[3]),
                                   tostring(raised[4])))
            kast.gui.setValue("log", nil)
            kast.log(string.format("lists: CLEARED log=%s", show(kast.gui.value("log"))))
        elseif frame == 4 then
            kast.gui.scrollTo("big", 49999)
            kast.gui.scrollTo("classes", 7)
            kast.gui.scrollTo("big", 100000) -- no such row: logged
            kast.gui.scrollTo("nowhere", 1) -- no such list: logged
            kast.log("lists: SCROLLTO")
        elseif frame == 5 then
            kast.gui.focus("big")
            kast.log("lists: KEYS")
        elseif frame == 150 then
            kast.gui.scrollToEnd("big")
        elseif frame >= 160 and frame <= 200 and frame % 20 == 0 then
            addItem()
        elseif frame == 220 then
            kast.gui.scrollTo("big", 49999)
        elseif frame == 240 then
            addItem()
            kast.log("lists: APPEND kept")
        end
        if frame % 15 == 0 and frame <= 240 then
            addLine()
        end
    end,
}
