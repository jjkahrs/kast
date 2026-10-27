-- Additive scenes in kast_player (player_additive_scenes): loads additive_b.scene on top of this one, makes it active,
-- then unloads it again, logging the loaded set at each step.
local frame = 0
local function loaded()
    return table.concat(kast.loadedScenes(), ",") .. " active " .. tostring(kast.activeScene())
end
return {
    init = function() kast.log("additive player: start " .. loaded()) end,
    update = function(dt)
        frame = frame + 1
        if frame == 2 then
            kast.loadScene("additive_b.scene", {additive = true})
        elseif frame == 3 then
            kast.log("additive player: loaded " .. loaded())
            kast.setActiveScene("additive_b.scene")
        elseif frame == 4 then
            kast.log("additive player: activated " .. loaded())
            kast.unloadScene("additive_b.scene")
        elseif frame == 5 then
            kast.log("additive player: unloaded " .. loaded())
        end
    end,
}
