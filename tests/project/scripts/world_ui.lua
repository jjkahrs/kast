-- Drives world_ui.scene (ctest world_ui_*, docs/engine/world-ui.html): kast.gui calls with an entity first reach only
-- that entity's WorldUI panel. init: Near's #name reads "Alice" and Far's #plate gets the "far" class (cyan), the
-- other panels on ui/nameplate.rml stay magenta; a bad entity id raises. From 1 s of Play time the Mover jumps 1.5 m
-- right (with --fixed-dt, from frame 60), so its panel must follow it.
local t, mover = 0, nil

local function at(x, y) -- the WorldUI entity standing there
    for e in kast.query("WorldUI") do
        local p = kast.get(e, "Transform").position
        if math.abs(p.x - x) < 1e-3 and math.abs(p.y - y) < 1e-3 then
            return e
        end
    end
end

return {
    init = function()
        local near, far = at(-0.8, 0), at(-0.2, 0)
        mover = at(0, 3.5)
        local problems = {}
        if not (near and far and mover) then problems[#problems + 1] = "fixture entities not found" end
        kast.gui.setText(near, "name", "Alice")
        kast.gui.setAttribute(far, "plate", "class", "plate far")
        local ok, err = pcall(kast.gui.setText, 999999, "name", "x")
        if ok or not string.find(err, "invalid entity id 999999", 1, true) then
            problems[#problems + 1] = "a bad entity id did not raise: " .. tostring(err)
        end
        ok, err = pcall(kast.gui.setText, 999999, "name") -- one argument short of the entity form: the plain one
        if ok or not string.find(err, "kast.gui.setText: id must be a string, got 999999", 1, true) then
            problems[#problems + 1] = "the plain form's error changed: " .. tostring(err)
        end
        kast.log(#problems == 0 and "world_ui: PASS" or ("world_ui: FAIL " .. table.concat(problems, "; ")))
    end,
    update = function(dt)
        t = t + dt
        if t >= 1 then
            kast.set(mover, "Transform", {position = {x = 1.5, y = 3.5, z = 0}})
        end
    end,
}
