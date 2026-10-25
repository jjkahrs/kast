-- Drives every kast.gui scaling and kast.display function in kast_player (project gui: reference 640x360, match 0.5,
-- no OS display scale, a 640x360 window) and logs "scale api: PASS" or one "scale api FAIL: ..." line per failure.
-- It ends windowed at 800x600 with UI scale 1.5, which the user settings file keeps.
local frame, failures = 0, 0

local function expect(ok, what)
    if not ok then
        failures = failures + 1
        kast.log("scale api FAIL: " .. what)
    end
end

local function near(a, b) return math.abs(a - b) < 1e-3 end

local function fails(message, fn)
    local ok, err = pcall(fn)
    expect(not ok and tostring(err):find(message, 1, true) ~= nil,
           "expected the error '" .. message .. "', got " .. (ok and "none" or tostring(err)))
end

return {
    init = function() end,
    update = function(dt)
        frame = frame + 1
        if frame == 5 then
            expect(near(kast.gui.dpRatio(), 1), "dpRatio is 1 at the reference size, got " .. kast.gui.dpRatio())
            expect(kast.gui.scale() == 1, "scale() is the project's 1")
            local w, h, match = kast.gui.referenceResolution()
            expect(w == 640 and h == 360 and match == 0.5, "referenceResolution() is the project's 640, 360, 0.5")
            kast.gui.setScale(2)
            expect(kast.gui.scale() == 2 and near(kast.gui.dpRatio(), 2), "setScale(2) doubles the dp ratio")
            kast.gui.setScale(10)
            expect(kast.gui.scale() == 3, "setScale(10) is clamped to 3")
            kast.gui.setScale(0.1)
            expect(kast.gui.scale() == 0.5, "setScale(0.1) is clamped to 0.5")
            fails("kast.gui.setScale: scale must be a number, got", function() kast.gui.setScale("big") end)
            fails("kast.gui.setScale: scale must be a number, got nil", function() kast.gui.setScale() end)
            kast.gui.setScale(1)
            kast.gui.setReferenceResolution(320, 360, 0) -- the window is twice as wide, as high
            expect(near(kast.gui.dpRatio(), 2), "match 0 fits the width: 2")
            kast.gui.setReferenceResolution(320, 360, 1)
            expect(near(kast.gui.dpRatio(), 1), "match 1 fits the height: 1")
            kast.gui.setReferenceResolution(320, 360, 0.5)
            expect(near(kast.gui.dpRatio(), math.sqrt(2)), "match 0.5 blends in log space: sqrt(2)")
            kast.gui.setReferenceResolution(320, 360, 7)
            local _, _, clamped = kast.gui.referenceResolution()
            expect(clamped == 1, "match 7 is clamped to 1")
            kast.gui.setReferenceResolution(320, 360)
            local _, _, kept = kast.gui.referenceResolution()
            expect(kept == 1, "a nil match keeps the last one")
            fails("kast.gui.setReferenceResolution: width must be a whole number from 1 to 16384, got 0",
                  function() kast.gui.setReferenceResolution(0, 360) end)
            fails("kast.gui.setReferenceResolution: height must be a whole number from 1 to 16384, got -5",
                  function() kast.gui.setReferenceResolution(640, -5) end)
            fails("kast.gui.setReferenceResolution: width must be a number, got",
                  function() kast.gui.setReferenceResolution("a", 1) end)
            fails("kast.gui.setReferenceResolution: match must be a number, got",
                  function() kast.gui.setReferenceResolution(1, 1, "x") end)
            kast.gui.setReferenceResolution(nil)
            expect(kast.gui.referenceResolution() == nil and near(kast.gui.dpRatio(), 1), "setReferenceResolution(nil) unsets it")
            kast.gui.setReferenceResolution(640, 360, 0.5)

            expect(kast.display.displayScale() > 0, "displayScale() is positive")
            local mode = kast.display.mode()
            expect(mode.mode == "windowed" and mode.width == 640 and mode.height == 360 and mode.refresh == nil,
                   "mode() is windowed 640x360")
            local modes = kast.display.modes()
            expect(#modes > 0 and modes[1].width > 0 and modes[1].height > 0 and modes[1].refresh > 0,
                   "modes() lists the display's exclusive modes")
            expect(kast.display.setMode({mode = "exclusive", width = 123, height = 45}) == false,
                   "an exclusive mode the display lacks is refused")
            expect(kast.display.setMode({mode = "fullscreen"}) == false, "an unknown mode name is refused")
            expect(kast.display.mode().mode == "windowed", "the mode is kept after a refusal")
            fails("kast.display.setMode: expected a table", function() kast.display.setMode("windowed") end)
            fails("kast.display.setMode: width must be a whole number from 1 to 16384, got 0",
                  function() kast.display.setMode({width = 0}) end)
            fails("kast.display.setMode: mode must be a string, got 3", function() kast.display.setMode({mode = 3}) end)
            fails("kast.display.setMode: unknown field", function() kast.display.setMode({colour = 1}) end)
            expect(kast.display.setMode({mode = "windowed", width = 800, height = 600}) == true, "windowed 800x600 is set")
        elseif frame == 10 then
            local mode = kast.display.mode()
            expect(mode.mode == "windowed" and mode.width == 800 and mode.height == 600, "mode() follows: windowed 800x600")
            expect(near(kast.gui.dpRatio(), math.sqrt(800 / 640 * 600 / 360)),
                   "the resize recomputes the dp ratio, got " .. kast.gui.dpRatio())
            kast.gui.setScale(1.5)
            kast.log(failures == 0 and "scale api: PASS" or "scale api: FAIL")
        end
    end,
}
