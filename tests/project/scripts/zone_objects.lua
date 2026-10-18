-- Zone objects on the hills (maps/hills/objects): two characters walk +x from 1 s on. The Hero runs into the obstacle
-- crate at (266, 248) (a 2 m box: x 265..267), the Walker through the visual crate at (248, 244). At 5 s their x and
-- what kast.nearestInteractable / kast.interact give are logged, for the self-check and zone_objects_play_screenshot
-- (docs/engine/terrain.html#objects).
local t, reported = 0, false
local hero, walker

return {
    update = function(dt)
        t = t + dt
        if not hero then
            for e in kast.query("CharacterController") do
                if kast.get(e, "Transform").position.z > 246 then hero = e else walker = e end
            end
        end
        if t >= 1 and t < 5 then
            kast.physics.moveCharacter(hero, {x = 2, y = 0, z = 0})
            kast.physics.moveCharacter(walker, {x = 2, y = 0, z = 0})
        elseif t >= 5 and not reported then
            reported = true
            kast.physics.moveCharacter(hero, {x = 0, y = 0, z = 0})
            kast.physics.moveCharacter(walker, {x = 0, y = 0, z = 0})
            kast.log(string.format("zone objects: hero x %.2f, walker x %.2f", kast.get(hero, "Transform").position.x,
                                   kast.get(walker, "Transform").position.x))
            local near = kast.nearestInteractable({x = 250, y = 0, z = 268.5}, 3)
            kast.log(string.format("zone objects: nearest %s %s %.2f m at %.0f, %.0f", tostring(near and near.id),
                                   tostring(near and near.name), near and near.distance or -1, near and near.pos.x or 0,
                                   near and near.pos.z or 0))
            kast.log("zone objects: none within 3 m of (250, 280): " .. tostring(kast.nearestInteractable({x = 250, y = 0, z = 280}, 3) == nil))
            kast.log("zone objects: interact offline " .. tostring(kast.interact(1)))
            local ok, err = pcall(kast.interact, 0)
            kast.log("zone objects: interact(0) " .. tostring(ok) .. " " .. tostring(err))
        end
    end,
}
