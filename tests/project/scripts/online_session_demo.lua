-- Online Services sample: logs the login the editor's Online panel hands to Play (Window > Online > Test login).
-- Play starts logged in as that account (kast.auth.account() is set from the first update; auth.session is delivered
-- with that frame's events); without one it starts logged out. Needs a project with "online" enabled (docs/engine/online-services.html).
local reported = false

return {
    init = function()
        if not kast.auth then
            return kast.error("kast.auth is nil: enable Online Services in project.kast")
        end
        kast.subscribe("auth.session", function(e)
            kast.log(string.format("auth.session: %s (account %d, %s)", e.email, e.accountId, e.role))
        end)
    end,
    update = function(dt)
        if reported then return end
        reported = true
        local account = kast.auth.account()
        kast.log(account and ("Play started logged in as " .. account.email) or "Play started logged out")
    end,
}
