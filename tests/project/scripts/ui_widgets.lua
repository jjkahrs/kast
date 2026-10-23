-- Drives ui/widgets.rml (ctest ui_widgets_*, player_widgets_*): form widget change events, kast.gui.value / setValue,
-- and widgets in elements made by data-for, kast.gui.clone and kast.gui.create.
-- init: every widget event is logged with its id, value and the value's Lua type; model "wbind" gets level 40, amount 6,
-- loaded 0.6 and two options. Frame 2: a clone of the spinbox and a created slider, then the values read back (VALUES).
-- Frame 3: setValue clamps and validates (SETVALUE; none of it publishes an event), then puts the values back. Frames
-- 1-60: setValue drives the progress bar (frame / 80; PROGRESS reads it back at frame 60). Frame 5: the spinbox's input
-- takes focus and KEYS is logged (--sim-on-log: arrow keys follow); frame 40 focuses the dropdown, frame 60 the slider.
-- Frame 100: the bound values (BOUND).
local frame = 0

local function show(value)
    return string.format("%s(%s)", tostring(value), type(value))
end

return {
    init = function()
        for _, name in ipairs({"sound", "volume", "quality", "count", "scale", "level", "amount", "option", "made"}) do
            kast.subscribe("widgets." .. name, function(e)
                kast.log(string.format("widgets: widgets.%s %s = %s in %s", name, e.id, show(e.value), e.document))
            end)
        end
        kast.gui.set("wbind", "level", 40)
        kast.gui.set("wbind", "amount", 6)
        kast.gui.set("wbind", "loaded", 0.6)
        kast.gui.set("wbind", "options", {{name = "Music", on = true}, {name = "Voices", on = false}})
    end,
    update = function()
        frame = frame + 1
        if frame <= 60 then
            kast.gui.setValue("loading", frame / 80)
        end
        if frame == 2 then
            local made = {
                kast.gui.clone("count", "made_row", "count_copy"),
                kast.gui.create("made_row", "input", "made_range",
                                {type = "range", min = "0", max = "10", step = "1", value = "5", ["data-event"] = "widgets.made"}),
            }
            kast.log(string.format("widgets: VALUES sound=%s volume=%s quality=%s count=%s scale=%s loading=%s copy=%s made=%s %s %s",
                                   show(kast.gui.value("sound")), show(kast.gui.value("volume")),
                                   show(kast.gui.value("quality")), show(kast.gui.value("count")),
                                   show(kast.gui.value("scale")), show(kast.gui.value("loading")),
                                   show(kast.gui.value("count_copy")), show(kast.gui.value("made_range")),
                                   tostring(made[1]), tostring(made[2])))
        elseif frame == 3 then
            kast.gui.setValue("volume", 150)
            kast.gui.setValue("count", 7.4)
            kast.gui.setValue("scale", 0.3)
            kast.gui.setValue("quality", "high")
            kast.gui.setValue("quality", "ultra") -- no such option: logged, ignored
            kast.gui.setValue("sound", false)
            kast.gui.setValue("loading", 5)
            local raised = not pcall(function() kast.gui.setValue("volume", {}) end)
            kast.log(string.format("widgets: SETVALUE volume=%s count=%s scale=%s quality=%s sound=%s loading=%s raised=%s",
                                   show(kast.gui.value("volume")), show(kast.gui.value("count")),
                                   show(kast.gui.value("scale")), show(kast.gui.value("quality")),
                                   show(kast.gui.value("sound")), show(kast.gui.value("loading")), tostring(raised)))
            kast.gui.setValue("volume", 20)
            kast.gui.setValue("count", 3)
            kast.gui.setValue("scale", 1)
            kast.gui.setValue("quality", "medium")
            kast.gui.setValue("sound", true)
        elseif frame == 5 then
            kast.gui.focus("count_input")
            kast.log("widgets: KEYS")
        elseif frame == 40 then
            kast.gui.focus("quality")
        elseif frame == 60 then
            kast.gui.focus("volume")
            kast.log(string.format("widgets: PROGRESS loading=%s", show(kast.gui.value("loading"))))
        elseif frame == 100 then
            local options = kast.gui.get("wbind", "options")
            kast.log(string.format("widgets: BOUND level=%s amount=%s loaded=%s music=%s", show(kast.gui.get("wbind", "level")),
                                   show(kast.gui.get("wbind", "amount")), show(kast.gui.get("wbind", "loaded")),
                                   show(options[1].on)))
        end
    end,
}
