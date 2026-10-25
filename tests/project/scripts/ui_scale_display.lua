-- kast.display window modes in kast_player, on the display the window is on (ctest: the secondary one): borderless,
-- then the display's first exclusive mode (its largest), then windowed again. Logs "scale display: PASS" or
-- "scale display FAIL: ..." lines.
local frame, failures, exclusive = 0, 0, nil

local function expect(ok, what)
    if not ok then
        failures = failures + 1
        kast.log("scale display FAIL: " .. what)
    end
end

return {
    init = function() end,
    update = function(dt)
        frame = frame + 1
        if frame == 5 then
            expect(kast.display.setMode({mode = "borderless"}) == true, "borderless is set")
        elseif frame == 15 then
            local mode = kast.display.mode()
            kast.log(string.format("scale display: borderless %dx%d, dp ratio %g", mode.width, mode.height, kast.gui.dpRatio()))
            expect(mode.mode == "borderless", "mode() is borderless, got " .. mode.mode)
            exclusive = kast.display.modes()[1]
            expect(exclusive ~= nil, "the display has an exclusive mode")
            if exclusive then
                expect(kast.display.setMode({mode = "exclusive", width = exclusive.width, height = exclusive.height,
                                             refresh = exclusive.refresh}) == true, "its first exclusive mode is set")
            end
        elseif frame == 30 then
            local mode = kast.display.mode()
            kast.log(string.format("scale display: exclusive %dx%d@%s", mode.width, mode.height, tostring(mode.refresh)))
            expect(mode.mode == "exclusive" and exclusive ~= nil and mode.width == exclusive.width and
                   mode.height == exclusive.height and mode.refresh == exclusive.refresh, "mode() is that exclusive mode")
            expect(kast.display.setMode({mode = "windowed", width = 640, height = 360}) == true, "windowed is set again")
        elseif frame == 45 then
            local mode = kast.display.mode()
            expect(mode.mode == "windowed" and mode.width == 640 and mode.height == 360, "mode() is windowed 640x360 again")
            kast.log(failures == 0 and "scale display: PASS" or "scale display: FAIL")
        end
    end,
}
