-- Loading screen, in each zone scene (ui/loading.rml, over every other document): scripts/characters.lua shows it on
-- the Play click and puts the "loading" game machine (fsms/loading.lua) in "entering". The zone's documents load
-- anew, so this one keeps the screen up with the same text (the name from the server's welcome, as scripts/hud.lua
-- shows it) until the zone has rendered: its first frames are slow (models load on first draw, the clones replication
-- sends come in), so it goes on the first quick one. Arriving any other way (a portal, Play in the editor) hides it
-- at once.
local SETTLED = 0.25 -- s: a frame this quick has nothing left to load
local MAX_FRAMES = 10 -- hidden by then however slow the frames stay
local frames -- updates since the switch while the screen is up

return {
    init = function()
        if kast.fsm.state("loading") ~= "entering" then return kast.gui.setVisible("loading", false) end
        frames = 0
        kast.subscribe("server.welcome", function(e)
            kast.gui.setText("loading_text", "Entering the world as " .. e.name .. "...")
        end, {replay = true}) -- replay: sent once on entering, maybe before this scene loaded
    end,
    update = function(dt)
        if not frames then return end
        frames = frames + 1
        if dt >= SETTLED and frames < MAX_FRAMES then return end
        kast.log(string.format("loading: hidden after zone rendered (%d frames, the last %.3f s)", frames, dt))
        frames = nil
        kast.gui.setVisible("loading", false)
        kast.fsm.change("loading", "idle")
    end,
}
