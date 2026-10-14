-- Drives the character's Animator through the kast animation API (models_lua.scene). CTest checks the log lines.
local frame = 0

return {
    init = function()
        for e in kast.query("Animator") do
            local clips = kast.getAnimationClips(e)
            kast.log("clips: " .. #clips .. ", first " .. clips[1])
            kast.playAnimation(e, "Angry", {fade = 0.25, loop = true, speed = 1.5})
            local ok, err = pcall(kast.playAnimation, e, "No_Such_Clip")
            kast.log("unknown clip: " .. tostring(ok) .. " " .. tostring(err))
            ok, err = pcall(kast.getAnimationState, 999999)
            kast.log("bad entity: " .. tostring(ok) .. " " .. tostring(err))
        end
        for e in kast.query("Transform") do
            if not kast.has(e, "Animator") then
                local ok, err = pcall(kast.getAnimationClips, e)
                kast.log("no animator: " .. tostring(ok) .. " " .. tostring(err))
                break
            end
        end
    end,
    update = function(dt)
        frame = frame + 1
        if frame ~= 30 then
            return
        end
        for e in kast.query("Animator") do
            local state = kast.getAnimationState(e)
            local animator = kast.get(e, "Animator")
            kast.log(string.format("state: %s %.2f %s, speed %.1f", state.clip, state.time, tostring(state.playing),
                                   animator.speed))
            kast.playAnimation(e, "Idle_A", {fade = 0.5})
        end
    end,
}
