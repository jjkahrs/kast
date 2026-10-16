#include "auth_client.h"

#include "project.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace kast {
namespace {

using Json = nlohmann::json;

// The claims of a JWT (its middle, base64url part), or null. The signature isn't checked: the server checks it, and
// the client only reads who it is.
Json jwtClaims(const std::string& token)
{
    const size_t first = token.find('.');
    const size_t second = first == std::string::npos ? first : token.find('.', first + 1);
    if (second == std::string::npos)
        return nullptr;
    std::string text = token.substr(first + 1, second - first - 1);
    std::ranges::replace(text, '-', '+');
    std::ranges::replace(text, '_', '/');
    text.append((4 - text.size() % 4) % 4, '=');
    const std::optional<std::string> bytes = base64Decode(text);
    if (!bytes)
        return nullptr;
    Json claims = Json::parse(*bytes, nullptr, false);
    return claims.is_object() ? claims : Json();
}

bool isString(const Json& body, const char* key) { return body.contains(key) && body[key].is_string(); }

// Events for the calls that answer nothing but success.
const char* doneEvent(const std::string& op)
{
    if (op == "register")
        return "auth.registered";
    if (op == "resendVerification")
        return "auth.verificationResent";
    if (op == "verifyEmail")
        return "auth.emailVerified";
    if (op == "forgotPassword")
        return "auth.passwordResetSent";
    if (op == "resetPassword")
        return "auth.passwordReset";
    if (op == "logout")
        return "auth.loggedOut";
    return nullptr;
}

} // namespace

std::optional<AuthConfig> authConfig(const Project& project, std::string& error)
{
    error.clear();
    std::string caPem;
    if (!project.online.enabled || !loadOnlineCa(project, caPem, error))
        return std::nullopt;
    return AuthConfig{project.online.authUrl, std::move(caPem)};
}

std::chrono::milliseconds authRefreshDelay(std::chrono::seconds lifetime)
{
    using std::chrono::milliseconds;
    const milliseconds life = lifetime;
    return std::max(std::min(life * 4 / 5, life - milliseconds(60000)), life / 2);
}

AuthClient::AuthClient(Network& network) : network_(network) {}

void AuthClient::reset(std::optional<AuthConfig> config)
{
    config_ = std::move(config);
    if (config_)
        while (config_->url.ends_with('/'))
            config_->url.pop_back();
    session_.reset();
    refreshRequest_ = 0;
    loggingIn_ = false;
    pending_.clear();
    held_.clear();
    events_.clear();
}

uint64_t AuthClient::registerAccount(const std::string& email, const std::string& password)
{
    return call("register", "/v1/auth/register", {{"email", email}, {"password", password}});
}
uint64_t AuthClient::resendVerification(const std::string& email)
{
    return call("resendVerification", "/v1/auth/resend-verification", {{"email", email}});
}
uint64_t AuthClient::verifyEmail(const std::string& token)
{
    return call("verifyEmail", "/v1/auth/verify-email", {{"token", token}});
}
uint64_t AuthClient::forgotPassword(const std::string& email)
{
    return call("forgotPassword", "/v1/auth/forgot-password", {{"email", email}});
}
uint64_t AuthClient::resetPassword(const std::string& token, const std::string& password)
{
    return call("resetPassword", "/v1/auth/reset-password", {{"token", token}, {"password", password}});
}

uint64_t AuthClient::login(const std::string& email, const std::string& password)
{
    if (session_ || loggingIn_) // a second session would be orphaned, never logged out
        return fail({nextId_++, "login"}, "already_logged_in",
                    loggingIn_ ? "a login is already in progress" : "already logged in; log out first");
    loggingIn_ = true; // until it answers (onResult / onError)
    return call("login", "/v1/auth/login", {{"email", email}, {"password", password}});
}

uint64_t AuthClient::logout()
{
    const Call logout{nextId_++, "logout"};
    if (!session_)
        return fail(logout, "not_logged_in", "not logged in");
    const std::string refreshToken = session_->refreshToken;
    dropSession("not_logged_in"); // at once: the game is logged out even if the server can't be told
    request(logout, "POST", "/v1/auth/logout", {{"refreshToken", refreshToken}});
    return logout.id;
}

uint64_t AuthClient::me() { return authorized("me"); }
uint64_t AuthClient::realms() { return authorized("realms"); }

std::optional<std::string> AuthClient::accessToken() const
{
    return session_ ? std::optional(session_->accessToken) : std::nullopt;
}

Json AuthClient::account() const { return session_ ? session_->account : Json(); }

void AuthClient::adopt(AuthSession session)
{
    session_ = std::move(session);
    refreshRequest_ = 0; // a refresh of the old session, if any, is ignored when it answers
    refreshAt_ = Clock::now() + authRefreshDelay(session_->expiresIn);
    emit("auth.session", session_->account);
}

uint64_t AuthClient::call(const char* op, const char* path, Json body)
{
    const Call call{nextId_++, op};
    request(call, "POST", path, body);
    return call.id;
}

uint64_t AuthClient::authorized(const char* op)
{
    const Call call{nextId_++, op};
    if (!session_)
        return fail(call, "not_logged_in", "not logged in");
    if (refreshRequest_)
        held_.push_back(call); // the access token is about to change
    else
        request(call, "GET", call.op == "me" ? "/v1/me" : "/v1/realms");
    return call.id;
}

uint64_t AuthClient::request(const Call& call, const char* method, const char* path, const Json& body)
{
    NetRequestOptions options;
    options.method = method;
    options.url = config_->url + path;
    options.caPem = config_->caPem;
    if (!body.is_null()) {
        options.headers.emplace_back("Content-Type", "application/json");
        options.body = body.dump();
    }
    if (session_ && (call.op == "me" || call.op == "realms"))
        options.headers.emplace_back("Authorization", "Bearer " + session_->accessToken);
    try {
        const uint64_t id = network_.request(std::move(options));
        pending_[id] = call;
        return id;
    } catch (const std::invalid_argument& e) { // a bad base URL or CA; project.kast checks make this unlikely
        onError(call, 0, 0, "network_error", e.what());
        return 0;
    }
}

uint64_t AuthClient::fail(const Call& call, const std::string& code, const std::string& message)
{
    onError(call, 0, 0, code, message);
    return call.id;
}

void AuthClient::emit(std::string name, Json payload) { events_.push_back({std::move(name), std::move(payload)}); }

bool AuthClient::handle(const NetEvent& event)
{
    const uint64_t request = event.payload.value("id", uint64_t{0});
    const auto it = pending_.find(request);
    if (it == pending_.end())
        return false;
    const Call call = it->second;
    pending_.erase(it);
    if (event.name != "net.http.response") {
        onError(call, request, 0, "network_error", event.payload.value("message", "request failed"));
        return true;
    }
    const int status = event.payload.value("status", 0);
    const Json text = event.payload.value("body", Json());
    const Json body = text.is_string() && !text.get_ref<const std::string&>().empty()
                          ? Json::parse(text.get_ref<const std::string&>(), nullptr, false)
                          : Json();
    if (status >= 200 && status < 300) {
        onResult(call, request, body);
    } else if (body.contains("error") && isString(body["error"], "code")) {
        const Json& error = body["error"];
        onError(call, request, status, error["code"].get<std::string>(), error.value("message", ""),
                error.value("details", Json()));
    } else {
        onError(call, request, status, "http_error", std::format("HTTP {}", status));
    }
    return true;
}

void AuthClient::onResult(const Call& call, uint64_t request, const Json& body)
{
    if (const char* name = doneEvent(call.op))
        return emit(name, {{"id", call.id}});
    if (call.op == "refresh") {
        if (request != refreshRequest_)
            return; // logged out (or replaced) meanwhile
        refreshRequest_ = 0;
        if (!startSession(body))
            return onError(call, request, 200, "bad_response", "the refresh response is not a session");
        std::vector<Call> held;
        held.swap(held_);
        for (const Call& c : held)
            this->request(c, "GET", c.op == "me" ? "/v1/me" : "/v1/realms");
        return;
    }
    if (call.op == "login") {
        loggingIn_ = false;
        if (!startSession(body))
            return onError(call, request, 200, "bad_response", "the login response is not a session");
        Json payload = session_->account;
        payload["id"] = call.id;
        return emit("auth.login.ok", std::move(payload));
    }
    if (call.op == "me" && body.is_object())
        return emit("auth.me", {{"id", call.id}, {"account", body}});
    if (call.op == "realms" && body.contains("realms") && body["realms"].is_array())
        return emit("auth.realms", {{"id", call.id}, {"realms", body["realms"]}});
    onError(call, request, 200, "bad_response", std::format("unexpected {} response", call.op));
}

void AuthClient::onError(const Call& call, uint64_t request, int status, const std::string& code,
                         const std::string& message, const Json& details)
{
    if (call.op == "login")
        loggingIn_ = false;
    if (call.op == "refresh") {
        if (request != refreshRequest_)
            return;
        // ponytail: any failed refresh ends the session, a dropped connection too; retry with backoff if that hurts
        dropSession("not_logged_in");
        return emit("auth.session.expired", {{"code", code}, {"message", message}});
    }
    Json payload{{"id", call.id}, {"op", call.op}, {"status", status}, {"code", code}, {"message", message}};
    if (!details.is_null())
        payload["details"] = details;
    emit("auth.error", std::move(payload));
}

bool AuthClient::startSession(const Json& body)
{
    if (!isString(body, "accessToken") || !isString(body, "refreshToken") || !body.contains("expiresIn") ||
        !body["expiresIn"].is_number() || body["expiresIn"].get<double>() <= 0)
        return false;
    const Json claims = jwtClaims(body["accessToken"]);
    if (!isString(claims, "sub"))
        return false;
    AuthSession session;
    session.accessToken = body["accessToken"];
    session.refreshToken = body["refreshToken"];
    session.expiresIn = std::chrono::seconds(body["expiresIn"].get<int64_t>());
    try {
        session.account = {{"accountId", std::stoll(claims["sub"].get<std::string>())},
                           {"email", claims.value("email", "")},
                           {"role", claims.value("role", "")}};
    } catch (const std::exception&) { // a sub that isn't a number
        return false;
    }
    session_ = std::move(session);
    refreshAt_ = Clock::now() + authRefreshDelay(session_->expiresIn);
    return true;
}

void AuthClient::dropSession(const std::string& code)
{
    session_.reset();
    refreshRequest_ = 0;
    std::vector<Call> held;
    held.swap(held_);
    for (const Call& call : held)
        fail(call, code, "not logged in");
}

std::vector<NetEvent> AuthClient::poll(Clock::time_point now)
{
    if (session_ && !refreshRequest_ && now >= refreshAt_) // a refresh that can't start fails as request 0, not stale
        refreshRequest_ = request({0, "refresh"}, "POST", "/v1/auth/refresh", {{"refreshToken", session_->refreshToken}});
    std::vector<NetEvent> out;
    out.swap(events_);
    return out;
}

} // namespace kast
