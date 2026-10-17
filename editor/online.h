#pragma once

#include <auth_client.h>
#include <network.h>
#include <project.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Process;

// The Online panel's model (docs/editor/online-tooling.html), no ImGui: the project's auth service health and
// realms with each realm's world server reachability, the local docker compose stack, the editor's test login (handed
// to each Play session) and the admin calls. Everything is asynchronous on its own Network; poll() every frame on the
// thread that made it. The login lives in memory only: passwords and tokens are never stored or logged. Remembers the
// email, compose folder and world override in the per-user editor settings (editor_settings.h).

// A check's result. Warning: up but something is off (auth's database down, a world server whose certificate isn't
// trusted); detail says what.
struct OnlineProbe {
    enum class State { Unknown, Checking, Ok, Warning, Down } state = State::Unknown;
    std::string detail;
};

struct OnlineRealm {
    int64_t id = 0;
    std::string name, host;
    uint16_t port = 0;
    std::string region, status;
    int sortOrder = 0;
    OnlineProbe world; // TLS connect to host:port
};

struct ComposeService {
    std::string name, state, health; // `docker compose ps` Service, State, Health ("" if none)
};

// An auth service error (or network_error / http_error), shown inline. Empty code = none.
struct OnlineError {
    std::string code, message;
};

// services/world under the Kast source tree this editor was built from, if its docker-compose.yml is there; else "".
std::string defaultComposeFolder();
// Why `docker compose` can't run: docker not on PATH, folder unset, missing or without docker-compose.yml. "" if it can.
std::string composeBlocker(bool dockerFound, const std::string& folder);
// The services in `docker compose ps --format json` output: a JSON array or one object per line (both compose
// versions). Lines that don't parse are skipped.
std::vector<ComposeService> parseComposePs(const std::string& output);

class OnlineModel {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kRefreshInterval{10}; // while the panel is visible
    static constexpr std::chrono::milliseconds kCheckTimeout{3000};
    enum class Compose { Up, Down, Ps };

    // settingsFile: editor_settings.json. composeDefault: the compose folder while none is saved.
    explicit OnlineModel(std::filesystem::path settingsFile, std::string composeDefault = defaultComposeFolder());
    ~OnlineModel(); // a docker command still running isn't killed, but loses its output pipe
    OnlineModel(const OnlineModel&) = delete;
    OnlineModel& operator=(const OnlineModel&) = delete;

    // Every frame. project: the open one (nullptr: none); when its root or online block changes the login and status
    // are dropped and, if enabled, checked again. panelVisible: refresh every kRefreshInterval while true.
    void poll(const kast::Project* project, bool panelVisible, Clock::time_point now = Clock::now());

    bool enabled() const { return config_.has_value(); } // the project has Online Services on and its CA file loads
    const std::string& configError() const { return configError_; } // why it is off despite being enabled

    // Status: auth GET /health, the realm list while logged in, then a TLS connect to each realm's world server.
    void refresh(Clock::time_point now = Clock::now());
    const OnlineProbe& authHealth() const { return health_; }
    const std::vector<OnlineRealm>& realms() const { return realms_; }
    std::optional<Clock::time_point> lastRefresh() const { return lastRefresh_; }

    // The local stack.
    const std::string& composeFolder() const { return composeFolder_; }
    bool setComposeFolder(std::string folder, std::string& error); // saved in the settings
    bool dockerFound() const { return dockerFound_; }
    std::string composeBlocker() const { return ::composeBlocker(dockerFound_, composeFolder_); }
    // Up: `docker compose up -d --build`, Down: `docker compose down` (never -v), Ps: `docker compose ps`. Output goes
    // to the log line by line; Up and Down then run Ps and refresh. False (nothing runs) if blocked or one is running.
    bool runCompose(Compose what);
    bool composeBusy() const { return compose_.process != nullptr; }
    const std::string& composeCommand() const { return compose_.label; } // the running (or last) command
    const std::string& composeError() const { return composeError_; }      // the last one's failure, "" if none
    const std::vector<ComposeService>& services() const { return services_; }

    // The test login.
    const std::string& email() const { return email_; } // remembered across editor runs
    // Remembers email, starts the login and wipes password.
    void login(const std::string& email, std::string& password);
    void logout();
    bool loggingIn() const { return loggingIn_; }
    bool loggedIn() const { return auth_.loggedIn(); }
    nlohmann::json account() const { return auth_.account(); } // {accountId, email, role}, null when logged out
    bool isAdmin() const;
    const OnlineError& loginError() const { return loginError_; }
    // Play hand-off (Document::onPlayStarted / onPlayStopping). lendSession: if logged in and play is enabled, play
    // adopts the login ("auth.session" reaches Lua) and the editor stops refreshing it. reclaimSession: at Stop, takes
    // back play's session, possibly refreshed meanwhile; if Play logged out, the editor is logged out too.
    void lendSession(kast::AuthClient& play);
    void reclaimSession(const kast::AuthClient& play);
    bool sessionLent() const { return lent_; }

    // Editor Play's world address override, host:port or "" for none. Saved in the settings.
    const std::string& worldOverride() const { return worldOverride_; }
    bool setWorldOverride(std::string hostPort, std::string& error);

    // Admin calls (need an admin login). One at a time; a success refreshes the realms.
    // input: RealmInput {name, host, port, region, status, sortOrder}.
    void createRealm(const nlohmann::json& input);
    void updateRealm(int64_t id, const nlohmann::json& input);
    void deleteRealm(int64_t id);
    void ban(int64_t accountId, const std::string& reason, const std::string& expiresAt); // expiresAt "" = permanent
    void unban(int64_t accountId);
    void setRole(int64_t accountId, const std::string& role);
    struct AdminResult {
        std::string op; // "createRealm", ...; "" before the first call
        bool busy = false, ok = false;
        OnlineError error;
    };
    const AdminResult& adminResult() const { return admin_; }

private:
    struct ComposeRun {
        SDL_Process* process = nullptr;
        Compose what = Compose::Ps;
        std::string label, output, partial; // output: Ps's whole output; partial: an unfinished line
    };

    void reconfigure(const kast::Project* project);
    void onAuthEvent(const kast::NetEvent& event);
    void onNetEvent(const kast::NetEvent& event);
    void checkWorlds();
    void admin(const char* op, const char* method, const std::string& path, const nlohmann::json& body = nullptr);
    void pumpCompose();
    void composeLine(std::string line);
    bool saveSetting(const char* key, const std::string& value, std::string& error);

    std::filesystem::path settingsFile_;
    kast::Network network_;
    kast::AuthClient auth_{network_};
    std::optional<std::pair<std::filesystem::path, kast::OnlineConfig>> project_; // what config_ was made from
    std::optional<kast::AuthConfig> config_;
    std::string configError_;

    OnlineProbe health_;
    uint64_t healthRequest_ = 0;
    std::vector<OnlineRealm> realms_;
    std::unordered_map<std::string, int64_t> worldChecks_; // connection name -> realm id
    std::optional<Clock::time_point> lastRefresh_;

    std::string composeFolder_;
    bool dockerFound_ = false;
    ComposeRun compose_;
    std::string composeError_;
    std::vector<ComposeService> services_;

    std::string email_;
    bool loggingIn_ = false;
    OnlineError loginError_;
    bool lent_ = false;

    std::string worldOverride_;

    AdminResult admin_;
    uint64_t adminRequest_ = 0;
};
