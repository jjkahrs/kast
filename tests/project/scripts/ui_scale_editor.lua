-- kast.display and kast.gui scaling in the editor's Play (ctest: --game-resolution 1280x720 --game-display-scale 1.5,
-- tests/project has no "gui" block): setMode only records, mode() is the Game view's size. Logs "scale editor: PASS"
-- or "scale editor FAIL: ..." lines.
local frame, failures = 0, 0

local function expect(ok, what)
    if not ok then
        failures = failures + 1
        kast.log("scale editor FAIL: " .. what)
    end
end

local function near(a, b) return math.abs(a - b) < 1e-3 end

return {
    init = function() end,
    update = function(dt)
        frame = frame + 1
        if frame == 10 then
            expect(near(kast.display.displayScale(), 1.5), "displayScale() is the simulated 1.5, got " .. kast.display.displayScale())
            expect(near(kast.gui.dpRatio(), 1.5), "dpRatio() is the display scale, got " .. kast.gui.dpRatio())
            expect(kast.display.setMode({mode = "borderless"}) == true, "setMode is accepted")
            local mode = kast.display.mode()
            expect(mode.mode == "borderless" and mode.width == 1280 and mode.height == 720,
                   string.format("mode() is the recorded mode at the Game view's size, got %s %dx%d", mode.mode,
                                 mode.width, mode.height))
            kast.gui.setScale(2)
            expect(near(kast.gui.dpRatio(), 3), "setScale(2) doubles it, got " .. kast.gui.dpRatio())
            kast.log(failures == 0 and "scale editor: PASS" or "scale editor: FAIL")
        end
    end,
}
