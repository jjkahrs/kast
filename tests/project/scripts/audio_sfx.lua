-- Sample kast.audio use (see README "Audio"): audio.scene's Music entity loops on the Music bus (AudioSource
-- playOnStart); this system fires one-shot SFX and clicks the Jukebox's AudioSource (UI bus).
local JUKEBOX = 2
local elapsed, shots = 0, 0

return {
    init = function()
        local h = kast.audio.play("sounds/mp3-demo.mp3", {bus = "SFX", volume = 0.8})
        kast.log(string.format("audio_sfx: one-shot started, playing=%s", tostring(kast.audio.isPlaying(h))))
    end,
    update = function(dt)
        elapsed = elapsed + dt
        if elapsed >= 0.5 then -- a short tone twice a second, alternating SFX and the Jukebox source
            elapsed = 0
            shots = shots + 1
            if shots % 2 == 0 then
                kast.audio.play("sounds/tone.wav", {volume = 0.4})
            else
                kast.audio.playSource(JUKEBOX)
            end
        end
    end,
}
