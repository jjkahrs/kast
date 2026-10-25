-- Drives ui/prefab_host.rml (ctest ui_prefab_*, player_guiprefab_*): frame 2 spawns an instance of
-- ui/prefab_button.guiprefab under #spawn with kast.gui.instantiate (INSTANTIATED) and tries the refusals (a missing
-- prefab, an unknown parent, a taken id); every prefab.clicked a button publishes is logged (CLICKED).
local frame = 0

return {
    init = function()
        kast.subscribe("prefab.clicked", function(e)
            kast.log(string.format("prefab: CLICKED id=%s document=%s", e.id, e.document))
        end)
    end,
    update = function()
        frame = frame + 1
        if frame == 2 then
            kast.log(string.format("prefab: INSTANTIATED %s", tostring(kast.gui.instantiate("ui/prefab_button.guiprefab", "spawn", "s"))))
            kast.log(string.format("prefab: REFUSED missing=%s parent=%s taken=%s",
                tostring(kast.gui.instantiate("ui/prefab_gone.guiprefab", "spawn", "t")),
                tostring(kast.gui.instantiate("ui/prefab_button.guiprefab", "nowhere", "t")),
                tostring(kast.gui.instantiate("ui/prefab_button.guiprefab", "spawn", "s"))))
            kast.gui.setText("s_title", "Spawned card")
        end
    end,
}
