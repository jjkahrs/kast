#include "online.h"

#include "assets.h" // fromUtf8
#include "editor_settings.h"

#include <log.h>
#include <world_client.h> // parseHostPort

#include <SDL3/SDL.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>
#include <system_error>

namespace {

using Json = nlohmann::json;
using State = OnlineProbe::State;

constexpr const char* kEmailKey = "onlineEmail";
constexpr const char* kComposeKey = "composeFolder";
constexpr const char* kWorldOverrideKey = "worldOverride";
constexpr const char* kWorldCheckPrefix = "world-check-";

bool onPath(const wchar_t* exe) { return SearchPathW(nullptr, exe, nullptr, 0, nullptr, nullptr) != 0; }

// The service's {error: {code, message}}, else http_error.
OnlineError httpError(int status, const std::string& text)
{
    const Json body = Json::parse(text, nullptr, false);
    if (body.is_object() && body.contains("error") && body["error"].is_object() && body["error"].contains("code") &&
        body["error"]["code"].is_string())
        return {body["error"]["code"].get<std::string>(), body["error"].value("message", "")};
    return {"http_error", std::format("HTTP {}", status)};
}

std::vector<OnlineRealm> parseRealms(const Json& list)
{
    std::vector<OnlineRealm> realms;
    for (const Json& r : list) {
        if (!r.is_object())
            continue;
        OnlineRealm realm;
        realm.id = r.value("id", int64_t{0});
        realm.name = r.value("name", "");
        realm.host = r.value("host", "");
        realm.port = static_cast<uint16_t>(std::clamp(r.value("port", 0), 0, 65535));
        realm.region = r.value("region", "");
        realm.status = r.value("status", "");
        realm.sortOrder = r.value("sortOrder", 0);
        realms.push_back(std::move(realm));
    }
    return realms;
}

std::string composeLabel(OnlineModel::Compose what)
{
    switch (what) {
    case OnlineModel::Compose::Up:
        return "docker compose up -d --build";
    case OnlineModel::Compose::Down:
        return "docker compose down";
    case OnlineModel::Compose::Ps:
        break;
    }
    return "docker compose ps";
}

} // namespace

std::string defaultComposeFolder()
{
#ifdef KAST_SOURCE_DIR
    // ponytail: only an editor run from its own build tree finds this; an installed one starts unset (Preferences).
    const std::filesystem::path folder = fromUtf8(KAST_SOURCE_DIR) / "services" / "world";
    std::error_code ec;
    if (std::filesystem::is_regular_file(folder / "docker-compose.yml", ec))
        return utf8(folder.lexically_normal());
#endif
    return {};
}

std::string composeBlocker(bool dockerFound, const std::string& folder)
{
    if (!dockerFound)
        return "docker was not found on PATH: install Docker Desktop, then restart the editor";
    if (folder.empty())
        return "No local services compose folder: set one in File > Preferences";
    std::error_code ec;
    const std::filesystem::path dir = fromUtf8(folder);
    if (!std::filesystem::is_directory(dir, ec))
        return std::format("The compose folder '{}' does not exist", folder);
    if (!std::filesystem::is_regular_file(dir / "docker-compose.yml", ec))
        return std::format("The compose folder '{}' has no docker-compose.yml", folder);
    return {};
}

std::vector<ComposeService> parseComposePs(const std::string& output)
{
    std::vector<ComposeService> services;
    const auto add = [&](const Json& s) {
        if (s.is_object() && s.contains("Service") && s["Service"].is_string())
            services.push_back({s["Service"], s.value("State", ""), s.value("Health", "")});
    };
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        const Json parsed = Json::parse(line, nullptr, false);
        if (parsed.is_array())
            std::ranges::for_each(parsed, add);
        else
            add(parsed);
    }
    std::ranges::sort(services, {}, &ComposeService::name);
    return services;
}

OnlineModel::OnlineModel(std::filesystem::path settingsFile, std::string composeDefault)
    : settingsFile_(std::move(settingsFile)), dockerFound_(onPath(L"docker.exe"))
{
    const Json settings = loadEditorSettings(settingsFile_);
    email_ = editorSetting(settings, kEmailKey);
    composeFolder_ = editorSetting(settings, kComposeKey, std::move(composeDefault));
    worldOverride_ = editorSetting(settings, kWorldOverrideKey);
}

OnlineModel::~OnlineModel()
{
    if (compose_.process)
        SDL_DestroyProcess(compose_.process); // not killed, but its output pipe closes: docker may stop early
}

void OnlineModel::poll(const kast::Project* project, bool panelVisible, Clock::time_point now)
{
    const bool changed = project ? !project_ || project_->first != project->root || project_->second != project->online
                                 : project_.has_value();
    if (changed)
        reconfigure(project);
    if (panelVisible && enabled() && (!lastRefresh_ || now - *lastRefresh_ >= kRefreshInterval))
        refresh(now);
    for (const kast::NetEvent& event : network_.poll())
        if (!auth_.handle(event))
            onNetEvent(event);
    if (!lent_)
        for (const kast::NetEvent& event : auth_.poll(now))
            onAuthEvent(event);
    pumpCompose();
}

void OnlineModel::reconfigure(const kast::Project* project)
{
    network_.stop();
    config_.reset();
    configError_.clear();
    project_.reset();
    if (project) {
        project_.emplace(project->root, project->online);
        config_ = kast::authConfig(*project, configError_);
    }
    auth_.reset(config_);
    health_ = {};
    healthRequest_ = 0;
    realms_.clear();
    worldChecks_.clear();
    lastRefresh_.reset();
    loggingIn_ = false;
    loginError_ = {};
    admin_ = {};
    adminRequest_ = 0;
}

void OnlineModel::refresh(Clock::time_point now)
{
    lastRefresh_ = now;
    if (!enabled())
        return;
    if (!healthRequest_) {
        try {
            healthRequest_ = network_.request(
                {.method = "GET", .url = config_->url + "/health", .caPem = config_->caPem, .timeout = kCheckTimeout});
            health_.state = State::Checking;
        } catch (const std::invalid_argument& e) {
            health_ = {State::Down, e.what()};
        }
    }
    if (loggedIn() && !lent_)
        auth_.realms(); // checkWorlds follows the answer
    else if (!realms_.empty())
        checkWorlds(); // keep the last list (Play holds the login)
}

void OnlineModel::checkWorlds()
{
    for (OnlineRealm& realm : realms_) {
        const std::string name = kWorldCheckPrefix + std::to_string(realm.id);
        if (worldChecks_.contains(name))
            continue; // still checking
        try {
            network_.open({.name = name,
                           .host = realm.host,
                           .port = realm.port,
                           .tls = true,
                           .caPem = config_->caPem,
                           .timeout = kCheckTimeout});
            worldChecks_[name] = realm.id;
            realm.world.state = State::Checking;
        } catch (const std::invalid_argument& e) {
            realm.world = {State::Down, e.what()};
        }
    }
}

void OnlineModel::onNetEvent(const kast::NetEvent& event)
{
    const Json& p = event.payload;
    if (event.name == "net.http.response" || event.name == "net.http.error") {
        const uint64_t id = p.value("id", uint64_t{0});
        const int status = p.value("status", 0);
        const std::string body = p.contains("body") && p["body"].is_string() ? p["body"].get<std::string>() : "";
        if (id && id == healthRequest_) {
            healthRequest_ = 0;
            if (event.name == "net.http.error")
                health_ = {State::Down, p.value("message", "unreachable")};
            else if (status == 200)
                health_ = {State::Ok, "ok"};
            else if (status == 503)
                health_ = {State::Warning, "database down (" + httpError(status, body).code + ")"};
            else
                health_ = {State::Down, httpError(status, body).message};
        } else if (id && id == adminRequest_) {
            adminRequest_ = 0;
            admin_.busy = false;
            if (event.name == "net.http.error") {
                admin_.error = {"network_error", p.value("message", "request failed")};
            } else if (status >= 200 && status < 300) {
                admin_.ok = true;
                kast::log::info(std::format("Online: {} done", admin_.op));
                if (loggedIn())
                    auth_.realms();
            } else {
                admin_.error = httpError(status, body);
            }
            if (!admin_.ok)
                kast::log::warn(std::format("Online: {} failed: {} ({})", admin_.op, admin_.error.code, admin_.error.message));
        }
        return;
    }
    const std::string conn = p.value("conn", "");
    const auto check = worldChecks_.find(conn);
    if (check == worldChecks_.end() || (event.name != "net.open" && event.name != "net.error"))
        return; // net.closed after an error or our close
    const auto realm = std::ranges::find(realms_, check->second, &OnlineRealm::id);
    worldChecks_.erase(check);
    if (event.name == "net.open")
        network_.close(conn);
    if (realm == realms_.end())
        return;
    if (event.name == "net.open") {
        realm->world = {State::Ok, "reachable"};
    } else {
        const std::string message = p.value("message", "unreachable");
        realm->world = message.find("certificate verify failed") != std::string::npos
                           ? OnlineProbe{State::Warning, "TLS certificate not trusted: set the CA file in Online Services"}
                           : OnlineProbe{State::Down, message};
    }
}

void OnlineModel::onAuthEvent(const kast::NetEvent& event)
{
    const Json& p = event.payload;
    if (event.name == "auth.login.ok") {
        loggingIn_ = false;
        loginError_ = {};
        kast::log::info(std::format("Online: logged in as {} ({})", p.value("email", ""), p.value("role", "")));
        refresh();
    } else if (event.name == "auth.realms") {
        std::vector<OnlineRealm> realms = parseRealms(p["realms"]);
        for (OnlineRealm& realm : realms) // keep the last check until the new one answers
            if (const auto old = std::ranges::find(realms_, realm.id, &OnlineRealm::id); old != realms_.end())
                realm.world = old->world;
        realms_ = std::move(realms);
        checkWorlds();
    } else if (event.name == "auth.session.expired") {
        loginError_ = {p.value("code", ""), "Session expired: " + p.value("message", "")};
        realms_.clear();
        kast::log::warn("Online: the editor's session expired; log in again");
    } else if (event.name == "auth.error") {
        const OnlineError error{p.value("code", ""), p.value("message", "")};
        if (p.value("op", "") == "login") {
            loggingIn_ = false;
            loginError_ = error;
        }
        if (p.value("op", "") != "logout") // logout clears the session at once; the server not hearing of it is fine
            kast::log::warn(std::format("Online: {} failed: {} ({})", p.value("op", ""), error.code, error.message));
    }
}

bool OnlineModel::saveSetting(const char* key, const std::string& value, std::string& error)
{
    return saveEditorSetting(settingsFile_, key, value, error);
}

bool OnlineModel::setComposeFolder(std::string folder, std::string& error)
{
    if (!saveSetting(kComposeKey, folder, error))
        return false;
    composeFolder_ = std::move(folder);
    return true;
}

bool OnlineModel::runCompose(Compose what)
{
    if (composeBusy() || !composeBlocker().empty())
        return false;
    std::vector<const char*> args{"docker", "compose"};
    switch (what) {
    case Compose::Up:
        args.insert(args.end(), {"up", "-d", "--build"});
        break;
    case Compose::Down:
        args.push_back("down");
        break;
    case Compose::Ps:
        args.insert(args.end(), {"ps", "--all", "--format", "json"});
        break;
    }
    args.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, composeFolder_.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    compose_ = {SDL_CreateProcessWithProperties(props), what, composeLabel(what)};
    SDL_DestroyProperties(props);
    composeError_.clear();
    if (!compose_.process) {
        composeError_ = std::format("Cannot start {}: {}", compose_.label, SDL_GetError());
        kast::log::error(composeError_);
        return false;
    }
    if (what != Compose::Ps)
        kast::log::info(std::format("Running {} in {}", compose_.label, composeFolder_));
    return true;
}

void OnlineModel::composeLine(std::string line)
{
    std::erase(line, '\r');
    if (line.empty())
        return;
    if (compose_.what == Compose::Ps)
        compose_.output += line + '\n';
    else
        kast::log::info("docker: " + line);
}

void OnlineModel::pumpCompose()
{
    if (!compose_.process)
        return;
    SDL_IOStream* out = static_cast<SDL_IOStream*>(
        SDL_GetPointerProperty(SDL_GetProcessProperties(compose_.process), SDL_PROP_PROCESS_STDOUT_POINTER, nullptr));
    int code = 0;
    const bool exited = SDL_WaitProcess(compose_.process, false, &code); // before reading: then nothing is left behind
    char buffer[4096];
    for (size_t n; out && (n = SDL_ReadIO(out, buffer, sizeof buffer)) > 0;) {
        compose_.partial.append(buffer, n);
        for (size_t end; (end = compose_.partial.find('\n')) != std::string::npos;) {
            composeLine(compose_.partial.substr(0, end));
            compose_.partial.erase(0, end + 1);
        }
    }
    if (!exited)
        return;
    composeLine(std::exchange(compose_.partial, {}));
    SDL_DestroyProcess(compose_.process);
    compose_.process = nullptr;
    const Compose what = compose_.what;
    if (code != 0) {
        composeError_ = std::format("{} failed (exit code {})", compose_.label, code);
        kast::log::error(composeError_);
    } else if (what == Compose::Ps) {
        services_ = parseComposePs(compose_.output);
    } else {
        kast::log::info(std::format("{} done", compose_.label));
    }
    if (what != Compose::Ps) { // what changed: the services' states and the checks
        runCompose(Compose::Ps);
        refresh();
    }
}

void OnlineModel::login(const std::string& email, std::string& password)
{
    if (std::string error; email != email_ && !saveSetting(kEmailKey, email, error))
        kast::log::warn(error);
    email_ = email;
    loginError_ = {};
    if (!enabled()) {
        loginError_ = {"disabled", "Online Services are disabled for this project"};
    } else if (lent_) {
        loginError_ = {"playing", "Stop Play first: the Play session holds the login"};
    } else {
        loggingIn_ = true;
        auth_.login(email, password); // its errors come back as auth.error, through poll
    }
    SecureZeroMemory(password.data(), password.size());
    password.clear();
}

void OnlineModel::logout()
{
    if (loggedIn() && !lent_) {
        auth_.logout();
        realms_.clear();
        kast::log::info("Online: logged out");
    }
}

bool OnlineModel::isAdmin() const
{
    const Json account = auth_.account();
    return account.is_object() && account.value("role", "") == "admin";
}

void OnlineModel::lendSession(kast::AuthClient& play)
{
    std::optional<kast::AuthSession> session = lent_ || !play.enabled() ? std::nullopt : auth_.session();
    if (!session)
        return;
    auth_.reset(config_); // forgets it here without telling the server: it lives on in Play
    lent_ = true;
    play.adopt(std::move(*session));
}

void OnlineModel::reclaimSession(const kast::AuthClient& play)
{
    if (!lent_)
        return;
    lent_ = false;
    if (std::optional<kast::AuthSession> session = play.session()) {
        auth_.adopt(std::move(*session));
    } else {
        realms_.clear();
        kast::log::info("Online: Play ended logged out, so the editor is logged out too");
    }
}

bool OnlineModel::setWorldOverride(std::string hostPort, std::string& error)
{
    if (!hostPort.empty() && !kast::parseHostPort(hostPort)) {
        error = std::format("'{}' is not host:port", hostPort);
        return false;
    }
    if (!saveSetting(kWorldOverrideKey, hostPort, error))
        return false;
    worldOverride_ = std::move(hostPort);
    return true;
}

void OnlineModel::admin(const char* op, const char* method, const std::string& path, const Json& body)
{
    admin_ = {op};
    const std::optional<std::string> token = auth_.accessToken();
    if (!enabled() || !token || !isAdmin() || lent_) {
        admin_.error = {"not_admin", "Log in with an admin account first (and not during Play)"};
        return;
    }
    if (adminRequest_) {
        admin_.error = {"busy", "Another admin call is still running"};
        return;
    }
    kast::NetRequestOptions options{.method = method, .url = config_->url + path, .caPem = config_->caPem};
    options.headers.emplace_back("Authorization", "Bearer " + *token);
    if (!body.is_null()) {
        options.headers.emplace_back("Content-Type", "application/json");
        options.body = body.dump();
    }
    try {
        adminRequest_ = network_.request(std::move(options));
        admin_.busy = true;
    } catch (const std::invalid_argument& e) {
        admin_.error = {"network_error", e.what()};
    }
}

void OnlineModel::createRealm(const Json& input) { admin("createRealm", "POST", "/v1/admin/realms", input); }
void OnlineModel::updateRealm(int64_t id, const Json& input)
{
    admin("updateRealm", "PUT", std::format("/v1/admin/realms/{}", id), input);
}
void OnlineModel::deleteRealm(int64_t id) { admin("deleteRealm", "DELETE", std::format("/v1/admin/realms/{}", id)); }
void OnlineModel::ban(int64_t accountId, const std::string& reason, const std::string& expiresAt)
{
    admin("ban", "POST", std::format("/v1/admin/accounts/{}/ban", accountId),
          {{"reason", reason}, {"expiresAt", expiresAt.empty() ? Json() : Json(expiresAt)}});
}
void OnlineModel::unban(int64_t accountId)
{
    admin("unban", "DELETE", std::format("/v1/admin/accounts/{}/ban", accountId));
}
void OnlineModel::setRole(int64_t accountId, const std::string& role)
{
    admin("setRole", "PUT", std::format("/v1/admin/accounts/{}/role", accountId), {{"role", role}});
}
