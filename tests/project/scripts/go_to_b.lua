-- Switches to scene B on the first Play frame (the switch is applied at the end of that frame).
return {
    update = function(dt)
        kast.loadScene("scene_b.scene")
    end,
}
