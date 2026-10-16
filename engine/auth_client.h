#pragma once

#include "network.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// AuthClient: the player-facing calls of the auth service (services/auth/openapi.yaml) over a Network, plus the
// logged-in session, kept in memory only. Calls return an id at once; their results come back from poll() as
// "auth.*" events (README "Online Services"), echoing that id. The access token is refreshed before it expires, one
// refresh at a time; a failed refresh ends the session ("auth.session.expired"). No Lua, no EventBus: LuaSystems owns
// one per Play session, routes the Network results it owns to it and publishes what poll() returns. Never logs
// passwords, tokens or bodies. Same threading rule as Network: everything on the Network's owner thread.
namespace kast {

struct AuthConfig {
    std::string url;   // the auth service base URL (project.kast online.authUrl)
    std::string caPem; // extra trusted CA certificates (online.caFile's text), may be empty
};

struct Project;
// project's Online Services as an AuthConfig: nullopt with error empty while disabled, nullopt with error set if the
// CA file can't be loaded (loadOnlineCa).
std::optional<AuthConfig> authConfig(const Project& project, std::string& error);

// A logged-in session. account is {accountId, email, role}, read from the access token's claims.
struct AuthSession {
    std::string accessToken, refreshToken;
    std::chrono::seconds expiresIn{0}; // access token lifetime from when the session was received
    nlohmann::json account;
};

// When to refresh a token that lives `lifetime`: at 80% of it, or 60 s before expiry if that is earlier, but never
// before half of it (short test tokens).
std::chrono::milliseconds authRefreshDelay(std::chrono::seconds lifetime);

class AuthClient {
public:
    using Clock = std::chrono::steady_clock;

    explicit AuthClient(Network& network);

    // Forgets the session, every pending call and queued event, then uses config (nullopt: disabled). The caller stops
    // the Network's requests (LuaSystems::start / stop do).
    void reset(std::optional<AuthConfig> config = std::nullopt);
    bool enabled() const { return config_.has_value(); }

    // Each starts a call and returns its id (from 1). Requires enabled().
    uint64_t registerAccount(const std::string& email, const std::string& password);
    uint64_t resendVerification(const std::string& email);
    uint64_t verifyEmail(const std::string& token);
    uint64_t login(const std::string& email, const std::string& password);
    uint64_t logout();
    uint64_t forgotPassword(const std::string& email);
    uint64_t resetPassword(const std::string& token, const std::string& password);
    uint64_t me();
    uint64_t realms();

    bool loggedIn() const { return session_.has_value(); }
    // The current access token, for engine code calling other services (world server). nullopt when logged out.
    std::optional<std::string> accessToken() const;
    // {accountId, email, role}, or null when logged out.
    nlohmann::json account() const;
    // Starts from a session obtained elsewhere (the editor's own login, at Play start): replaces any session and
    // queues "auth.session" {accountId, email, role}.
    void adopt(AuthSession session);

    // Takes a Network result if it answers one of this client's requests (true), else leaves it alone (false).
    bool handle(const NetEvent& event);
    // Starts a due refresh, then returns the events since the last poll, oldest first.
    std::vector<NetEvent> poll(Clock::time_point now = Clock::now());

private:
    struct Call {
        uint64_t id;
        std::string op; // "login", "me", ... ("refresh" for the client's own)
    };

    // Starts call's request; returns the Network request id, or 0 if it couldn't start (its error is queued).
    uint64_t request(const Call& call, const char* method, const char* path, const nlohmann::json& body = nullptr);
    uint64_t call(const char* op, const char* path, nlohmann::json body);  // a POST that needs no session
    uint64_t authorized(const char* op);                                   // me, realms: GET with the access token
    uint64_t fail(const Call& call, const std::string& code, const std::string& message); // queues auth.error
    void emit(std::string name, nlohmann::json payload);
    bool startSession(const nlohmann::json& body); // from a Session response body; false if malformed
    void dropSession(const std::string& code);      // logged out: held calls fail with code
    void onResult(const Call& call, uint64_t request, const nlohmann::json& body);
    void onError(const Call& call, uint64_t request, int status, const std::string& code, const std::string& message,
                 const nlohmann::json& details = nullptr);

    Network& network_;
    std::optional<AuthConfig> config_;
    std::optional<AuthSession> session_;
    Clock::time_point refreshAt_{};
    uint64_t refreshRequest_ = 0; // the Network request of the refresh in flight, 0 if none
    bool loggingIn_ = false;
    uint64_t nextId_ = 1;
    std::unordered_map<uint64_t, Call> pending_; // by Network request id
    std::vector<Call> held_;                     // me / realms waiting for the refresh in flight
    std::vector<NetEvent> events_;
};

} // namespace kast
