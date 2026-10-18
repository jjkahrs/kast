-- Reacts to ui/drag.rml (ctest ui_drag_*): logs the kast.gui drag events and what the mouse hovers, and lights the drop
-- target's border (kast.gui.setAttribute) when the box is dropped on it.
local lastHover
return {
    init = function()
        kast.subscribe("test.box.start", function(e)
            kast.log(string.format("Drag start: %s in %s", e.id, e.document))
        end)
        kast.subscribe("test.box.drop", function(e)
            kast.log(string.format("Drag drop: %s onto '%s' in '%s' shift=%s", e.id, e.target, e.targetDocument,
                tostring(e.shift)))
            if e.target == "target" then kast.gui.setAttribute("target", "class", "target lit") end
        end)
    end,
    update = function()
        local hover = kast.gui.hovered()
        if hover ~= lastHover then
            lastHover = hover
            kast.log("Hovered: " .. tostring(hover))
        end
    end,
}
