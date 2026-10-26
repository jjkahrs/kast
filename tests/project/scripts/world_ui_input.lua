-- Drives world_ui_input.scene (ctest world_ui_input_*, docs/engine/world-ui.html): logs every click a ui/wui_buttons.rml
-- panel publishes ("wui: right on angled", by the panel's entity), the screen GUI's button ("wui: screen"), and each
-- change of kast.gui.mouseOver() ("wui: mouseOver true"), so the tests read where simulated clicks landed. init checks
-- kast.gui.worldText's argument errors and spawn cap, then shows a "1234" damage text and a green test block
-- (ui/world_text.rcss) for 1 s; the floating text count is logged as it changes ("wui: worldText count 0 at 1.00").
local t, labels, over, count = 0, {}, nil, nil

local function at(x, y) -- the WorldUI entity standing there
    for e in kast.query("WorldUI") do
        local p = kast.get(e, "Transform").position
        if math.abs(p.x - x) < 1e-3 and math.abs(p.y - y) < 1e-3 then
            return e
        end
    end
end

local function clicked(event)
    kast.log(string.format("wui: %s on %s", event.id, labels[event.entity] or tostring(event.entity)))
end

return {
    init = function()
        for name, p in pairs({overlay = {-2.5, 3}, passive = {2.5, 3}, angled = {0, 1.5}, behind = {3, 0.7},
                              under = {-3, 0.7}}) do
            local e = at(p[1], p[2])
            if e then labels[e] = name end
        end
        kast.subscribe("wui.left", clicked)
        kast.subscribe("wui.right", clicked)
        kast.subscribe("wui.screen", function() kast.log("wui: screen") end)

        local problems = {}
        local function raises(expected, ...)
            local ok, err = pcall(kast.gui.worldText, ...)
            if ok or not string.find(tostring(err), expected, 1, true) then
                problems[#problems + 1] = "expected '" .. expected .. "', got " .. tostring(err)
            end
        end
        local pos = {x = 0, y = 3.3, z = 0}
        raises("kast.gui.worldText: pos must be a table {x, y, z} of numbers, got 5", 5, "t", "damage", 1)
        raises("kast.gui.worldText: pos must be a table {x, y, z} of numbers", {x = 0, y = 1}, "t", "damage", 1)
        raises("kast.gui.worldText: text must be a string, got nil", pos, nil, "damage", 1)
        raises("kast.gui.worldText: class must be a string, got 3", pos, "t", 3, 1)
        raises("kast.gui.worldText: lifetime must be a positive number of seconds, got 0", pos, "t", "damage", 0)
        raises("kast.gui.worldText: lifetime must be a positive number of seconds, got x", pos, "t", "damage", "x")
        for i = 1, 300 do -- behind the camera (never drawn), gone after the first frame
            kast.gui.worldText({x = 0, y = 0, z = 20}, tostring(i), "damage", 0.001)
        end
        if kast.gui.worldTextCount() ~= 256 then
            problems[#problems + 1] = "300 spawns left " .. kast.gui.worldTextCount() .. ", expected the cap 256"
        end
        kast.gui.worldText(pos, "1234", "damage", 1)
        kast.gui.worldText({x = 0, y = 2.6, z = 0}, "", "wui-block", 1)
        kast.log(#problems == 0 and "world_ui_input: PASS" or ("world_ui_input: FAIL " .. table.concat(problems, "; ")))
    end,
    update = function(dt)
        t = t + dt
        local now = kast.gui.mouseOver()
        if now ~= over then
            over = now
            kast.log("wui: mouseOver " .. tostring(now))
        end
        local n = kast.gui.worldTextCount()
        if n ~= count then
            count = n
            kast.log(string.format("wui: worldText count %d at %.2f", n, t))
        end
    end,
}
