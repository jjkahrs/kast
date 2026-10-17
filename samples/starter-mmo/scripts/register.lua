-- Register screen (scenes/register.scene, ui/register.rml): 1. email + password -> kast.auth.register; the service
-- mails a verification token (in the dev stack every mail lands in Mailpit, http://localhost:8026). 2. the pasted token
-- -> kast.auth.verifyEmail. Then back to login. The password field is cleared as soon as it is read.
local ERRORS = {
    invalid_token = "That token is wrong or expired. Resend the email for a new one.",
    rate_limited = "Too many requests. Wait a minute and try again.",
    network_error = "The auth service is unreachable. Is the compose stack running?",
}

local function status(text) kast.gui.setText("register_status", text) end

local function errorText(e)
    if e.code == "validation_error" and e.details and e.details[1] then
        return e.details[1].path .. ": " .. e.details[1].message
    end
    return ERRORS[e.code] or (e.code .. ": " .. e.message)
end

return {
    init = function()
        if not kast.auth then return status("Online Services are unavailable: the Console says why.") end
        kast.subscribe("register.submit", function()
            local email, password = kast.gui.value("email"), kast.gui.value("password")
            kast.gui.setText("password", "")
            if email == "" or password == "" then return status("Enter an email and a password.") end
            status("Registering...")
            kast.auth.register(email, password)
        end)
        kast.subscribe("register.verify", function()
            local token = kast.gui.value("token")
            if token == "" then return status("Paste the token from the email first.") end
            status("Verifying...")
            kast.auth.verifyEmail(token)
        end)
        kast.subscribe("register.resend", function()
            local email = kast.gui.value("email")
            if email == "" then return status("Enter your email first.") end
            kast.auth.resendVerification(email)
        end)
        kast.subscribe("register.back", function() kast.loadScene("scenes/login.scene") end)
        kast.subscribe("auth.registered", function() status("Check Mailpit (http://localhost:8026) for the token.") end)
        kast.subscribe("auth.verificationResent", function() status("Sent again: check Mailpit.") end)
        kast.subscribe("auth.emailVerified", function() status("Email verified. Back to login to play.") end)
        kast.subscribe("auth.error", function(e) status(errorText(e)) end)
    end,
    update = function(dt) end,
}
