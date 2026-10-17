-- Online Services sample: logs in through kast.auth, then lists the realms. Needs a project with "online" enabled
-- (docs/engine/online-services.html); the tests run it against kast_net_test_server's fake auth routes.
local EMAIL, PASSWORD = "demo@example.com", "demo-password-123"

return {
    init = function()
        if not kast.auth then
            return kast.error("kast.auth is nil: enable Online Services in project.kast")
        end
        kast.subscribe("auth.login.ok", function(e)
            kast.log(string.format("logged in as %s (account %d, %s)", e.email, e.accountId, e.role))
            kast.auth.realms()
        end)
        kast.subscribe("auth.realms", function(e)
            local names = {}
            for _, realm in ipairs(e.realms) do
                names[#names + 1] = string.format("%s [%s] %s:%d", realm.name, realm.status, realm.host, realm.port)
            end
            kast.log(string.format("%d realm(s): %s", #e.realms, table.concat(names, ", ")))
        end)
        kast.subscribe("auth.error", function(e)
            kast.error(string.format("%s failed: %s (%s)", e.op, e.code, e.message))
        end)
        kast.auth.login(EMAIL, PASSWORD)
    end,
    update = function(dt) end,
}
