# Makes sure the live tests' account (KAST_ONLINE_EMAIL / KAST_ONLINE_PASSWORD, read from the environment only) can log
# in to the compose stack's auth service: logs in; if the account is missing or unverified, registers it, reads the
# verification token from Mailpit's API and verifies it, then logs in again. Each login is logged out at once (its
# refresh token revoked). Never prints the password or a token. Test data only: a TEST stack account.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File starter_mmo_account.ps1
$ErrorActionPreference = 'Stop'
$auth = 'http://localhost:3100'
$mailpit = 'http://localhost:8026'
$email = $env:KAST_ONLINE_EMAIL
$password = $env:KAST_ONLINE_PASSWORD
# The stack trusts X-Forwarded-For (TEST ONLY, services/world/docker-compose.yml): a random one per run keeps auth's
# per-IP register limit (5/hour) from blocking reruns.
$headers = @{ 'X-Forwarded-For' = "10.77.$(Get-Random -Maximum 255).$(Get-Random -Maximum 255)" }

function Post($path, $body) {
    Invoke-RestMethod -Method Post -Uri "$auth$path" -Headers $headers -ContentType 'application/json' `
        -Body ($body | ConvertTo-Json -Compress)
}

function ErrorCode($err) {
    try { return ($err.ErrorDetails.Message | ConvertFrom-Json).error.code } catch { return 'unknown' }
}

function TryLogin {
    try {
        $session = Post '/v1/auth/login' @{ email = $email; password = $password }
        Post '/v1/auth/logout' @{ refreshToken = $session.refreshToken } | Out-Null
        return 'ok'
    } catch {
        return ErrorCode $_
    }
}

$code = TryLogin
if ($code -eq 'ok') { Write-Output "account ${email}: ready"; exit 0 }
if ($code -ne 'invalid_credentials' -and $code -ne 'email_not_verified') {
    Write-Output "account ${email}: login failed ($code)"; exit 1
}
$since = (Get-Date).ToUniversalTime().AddSeconds(-5)
if ($code -eq 'invalid_credentials') { Post '/v1/auth/register' @{ email = $email; password = $password } | Out-Null }
else { Post '/v1/auth/resend-verification' @{ email = $email } | Out-Null }

$token = $null
for ($i = 0; $i -lt 30 -and -not $token; $i++) {
    Start-Sleep -Milliseconds 500
    $list = Invoke-RestMethod -Uri "$mailpit/api/v1/search?query=$([uri]::EscapeDataString("to:$email"))"
    foreach ($m in $list.messages) {
        if (([datetime]$m.Created).ToUniversalTime() -lt $since) { continue } # an older mail's token is spent
        $text = (Invoke-RestMethod -Uri "$mailpit/api/v1/message/$($m.ID)").Text
        if ($text -match 'token=([A-Za-z0-9_-]+)') { $token = $Matches[1]; break }
    }
}
if (-not $token) { Write-Output "account ${email}: no verification email in Mailpit"; exit 1 }
Post '/v1/auth/verify-email' @{ token = $token } | Out-Null
$code = TryLogin
if ($code -ne 'ok') {
    # Registered before with another password: register can't tell (it never reveals an account exists).
    Write-Output "account ${email}: login failed after verification ($code); use another KAST_ONLINE_EMAIL"
    exit 1
}
Write-Output "account ${email}: registered and verified"
