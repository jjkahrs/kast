-- Drives ui/bind.rml (ctest ui_bind_*): kast.gui data models and runtime elements.
-- init: model "binds" gets a title, a count, open and 50 rows (each with a name and nested tags); model "form" the
-- two-way inputs' starting values. Frame 2: two clones of the hidden template and a created row, then BIND50 (the
-- screenshot of 50 rows). Frame 5: the text field takes focus and KEYS is logged (--sim-on-log: typed keys follow);
-- frame 40 focuses the range and frame 70 the select (arrow keys follow). Frame 60: 3 rows, one clone removed, BIND3.
-- Frame 100: logs what kast.gui.get reads back from the two-way inputs. A Pick click logs its button's id.
local frame = 0

local function rows(count)
    local list = {}
    for i = 1, count do
        list[i] = {name = "Row " .. i, tags = i % 2 == 0 and {"even"} or {"odd", "x"}}
    end
    return list
end

local function setRows(count)
    kast.gui.set("binds", "rows", rows(count))
    kast.gui.set("binds", "count", count)
end

return {
    init = function()
        kast.gui.set("binds", "title", "Bound list")
        kast.gui.set("binds", "open", true)
        setRows(50)
        kast.gui.set("form", "name", "")
        kast.gui.set("form", "agree", false)
        kast.gui.set("form", "volume", 5)
        kast.gui.set("form", "color", "red")
        kast.subscribe("bind.pick", function(e)
            kast.log(string.format("bind: picked %s in %s", e.id, e.document))
        end)
    end,
    update = function()
        frame = frame + 1
        if frame == 2 then
            local made = {
                kast.gui.clone("clone_template", "clone_list", "clone_1"),
                kast.gui.clone("clone_template", "clone_list", "clone_2"),
                kast.gui.create("clone_list", "div", "made_1", {class = "clone_row"}),
                kast.gui.create("made_1", "p", "made_1_name", {class = "clone_name"}),
            }
            kast.gui.setText("clone_1_name", "Clone 1")
            kast.gui.setText("clone_2_name", "Clone 2")
            kast.gui.setText("made_1_name", "Created")
            kast.log(string.format("bind: made %s %s %s %s, BIND50", tostring(made[1]), tostring(made[2]),
                                   tostring(made[3]), tostring(made[4])))
        elseif frame == 5 then
            kast.gui.focus("form_name")
            kast.log("bind: KEYS")
        elseif frame == 40 then
            kast.gui.focus("form_volume")
        elseif frame == 60 then
            setRows(3)
            kast.log(string.format("bind: removed clone_2 %s, BIND3", tostring(kast.gui.remove("clone_2"))))
        elseif frame == 70 then
            kast.gui.focus("form_color")
        elseif frame == 100 then
            kast.log(string.format("bind: two-way name=%s agree=%s volume=%s color=%s", tostring(kast.gui.get("form", "name")),
                                   tostring(kast.gui.get("form", "agree")), tostring(kast.gui.get("form", "volume")),
                                   tostring(kast.gui.get("form", "color"))))
        end
    end,
}
