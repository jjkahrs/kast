-- GUI scaling test scene (ui_scale.scene: ui/scale_dp.rml in dp mode, ui/scale_all.rml in all mode): logs the scale
-- and window it starts with, and clicks on the all-mode document's button.
return {
    init = function()
        local mode = kast.display.mode()
        kast.log(string.format("ui_scale: UI scale %g, %s %dx%d", kast.gui.scale(), mode.mode, mode.width, mode.height))
        kast.subscribe("scale.all", function(event)
            kast.log("Scale all clicked: " .. event.id .. " in " .. event.document)
        end)
    end,
    update = function(dt) end,
}
