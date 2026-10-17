-- Login screen (scenes/login.scene, ui/login.rml): email + password -> kast.auth.login, then the realm list. Play
-- started with the editor's test login (Window > Online > Test login, or --online-login-env) skips the form:
-- auth.session arrives before the first update. The password field is cleared as soon as it is read.
local REASONS = {
    disconnected = "You were disconnected from the world server.",
    expired = "Your session expired. Please log in again.",
}
local ERRORS = {
    invalid_credentials = "Wrong email or password.",
    email_not_verified = "Verify your email first (Create an account > step 2).",
    account_locked = "Too many attempts: the account is locked for a while.",
    account_banned = "This account is banned.",
    rate_limited = "Too many requests. Wait a minute and try again.",
    network_error = "The auth service is unreachable. Is the compose stack running?",
    validation_error = "Enter a valid email and password.",
}

local function status(text) kast.gui.setText("login_status", text) end

return {
    init = function()
        if not kast.auth then
            return status("Online Services are unavailable: the Console says why (missing CA file? see README).")
        end
        status(REASONS[kast.fsm.state("session")] or "")
        kast.fsm.change("session", "fresh")
        kast.subscribe("login.submit", function()
            local email, password = kast.gui.value("email"), kast.gui.value("password")
            kast.gui.setText("password", "")
            if email == "" or password == "" then return status("Enter your email and password.") end
            status("Logging in...")
            kast.auth.login(email, password)
        end)
        kast.subscribe("login.register", function() kast.loadScene("scenes/register.scene") end)
        kast.subscribe("auth.login.ok", function() kast.loadScene("scenes/realms.scene") end)
        kast.subscribe("auth.session", function() kast.loadScene("scenes/realms.scene") end)
        kast.subscribe("auth.error", function(e)
            if e.op == "login" then status(ERRORS[e.code] or (e.code .. ": " .. e.message)) end
        end)
    end,
    update = function(dt) end,
}
