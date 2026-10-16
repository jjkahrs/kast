#include "online_panel.h"

#include "ui_common.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <format>
#include <iterator>

namespace {

using State = OnlineProbe::State;

constexpr ImVec4 kWarnColor{1.0f, 0.8f, 0.0f, 1.0f}; // the palette's highlight, #ffcc00
constexpr const char* kRealmStatuses[] = {"online", "offline", "maintenance"};
constexpr const char* kRoles[] = {"player", "gm", "admin"};

void probe(const OnlineProbe& p)
{
    switch (p.state) {
    case State::Unknown:
        ImGui::TextDisabled("unknown");
        return;
    case State::Checking:
        ImGui::TextDisabled("checking...");
        return;
    case State::Ok:
        ImGui::TextColored(kOkColor, "%s", p.detail.c_str());
        return;
    case State::Warning:
        ImGui::PushStyleColor(ImGuiCol_Text, kWarnColor);
        ImGui::TextWrapped("%s", p.detail.c_str());
        ImGui::PopStyleColor();
        return;
    case State::Down:
        ImGui::TextColored(kErrorColor, "unreachable");
        ImGui::SetItemTooltip("%s", p.detail.c_str());
        return;
    }
}

void errorText(const OnlineError& error)
{
    if (error.code.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
    ImGui::TextWrapped("%s: %s", error.code.c_str(), error.message.c_str());
    ImGui::PopStyleColor();
}

void reason(const std::string& why)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", why.c_str());
    ImGui::PopStyleColor();
}

int indexOf(const char* const* list, int count, const std::string& value)
{
    for (int i = 0; i < count; ++i)
        if (value == list[i])
            return i;
    return 0;
}

} // namespace

void OnlinePanel::draw(const Document& doc, OnlineModel& online, FileActions& files,
                       const std::string& worldAddressFlag, bool& open)
{
    visible_ = false;
    if (!open)
        return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 40, ImGui::GetMainViewport()->WorkSize.y * 0.9f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Online", &open)) {
        visible_ = true;
        if (!doc.project) {
            ImGui::TextDisabled("Open a project to use Online Services");
        } else if (!doc.project->online.enabled) {
            ImGui::TextUnformatted("Online Services disabled for this project");
            ImGui::BeginDisabled(!files.canBuild());
            if (ImGui::Button("Online Services..."))
                files.openOnlineSettings();
            ImGui::EndDisabled();
            disabledTooltip("Not while playing or while another dialog is open");
        } else {
            if (!online.configError().empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::TextWrapped("Online Services: %s", online.configError().c_str());
                ImGui::PopStyleColor();
            }
            drawStatus(online);
            drawStack(online);
            drawLogin(online);
            drawWorldOverride(online, worldAddressFlag);
            if (online.isAdmin())
                drawAdmin(online);
        }
        drawConfirm();
    }
    ImGui::End();
}

void OnlinePanel::drawStatus(OnlineModel& online)
{
    ImGui::SeparatorText("Status");
    beginCard();
    if (ImGui::Button("Refresh"))
        online.refresh();
    if (const auto last = online.lastRefresh()) {
        ImGui::SameLine();
        const auto ago = std::chrono::duration_cast<std::chrono::seconds>(OnlineModel::Clock::now() - *last);
        ImGui::TextDisabled("checked %llds ago, every %llds while shown", static_cast<long long>(ago.count()),
                            static_cast<long long>(OnlineModel::kRefreshInterval.count()));
    }
    ImGui::TextUnformatted("Auth");
    ImGui::SameLine();
    probe(online.authHealth());
    if (online.realms().empty()) {
        ImGui::TextDisabled(online.loggedIn() || online.sessionLent() ? "No realms"
                                                                      : "Log in to list the realms and check their world servers");
    } else if (ImGui::BeginTable("realms", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const char* column : {"Realm", "Address", "Region", "Status"})
            ImGui::TableSetupColumn(column);
        ImGui::TableSetupColumn("World", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const OnlineRealm& realm : online.realms()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(realm.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s:%u", realm.host.c_str(), realm.port);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(realm.region.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(realm.status.c_str());
            ImGui::TableNextColumn();
            probe(realm.world);
        }
        ImGui::EndTable();
    }
    endCard();
}

void OnlinePanel::drawStack(OnlineModel& online)
{
    ImGui::SeparatorText("Local services (docker compose)");
    beginCard();
    ImGui::TextDisabled("Folder: %s", online.composeFolder().empty() ? "(not set)" : online.composeFolder().c_str());
    const std::string blocker = online.composeBlocker();
    const bool busy = online.composeBusy();
    ImGui::BeginDisabled(!blocker.empty() || busy);
    if (ImGui::Button("Start"))
        online.runCompose(OnlineModel::Compose::Up);
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
        online.runCompose(OnlineModel::Compose::Down);
    ImGui::SameLine();
    if (ImGui::Button("Status"))
        online.runCompose(OnlineModel::Compose::Ps);
    ImGui::EndDisabled();
    if (!blocker.empty())
        reason(blocker);
    else if (busy)
        ImGui::TextColored(kWarnColor, "Running %s... (output in the Console)", online.composeCommand().c_str());
    if (!online.composeError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", online.composeError().c_str());
        ImGui::PopStyleColor();
    }
    if (!online.services().empty() && ImGui::BeginTable("services", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* column : {"Service", "State", "Health"})
            ImGui::TableSetupColumn(column);
        ImGui::TableHeadersRow();
        for (const ComposeService& service : online.services()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(service.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(service.state == "running" ? kOkColor : kErrorColor, "%s", service.state.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(service.health.c_str());
        }
        ImGui::EndTable();
    }
    endCard();
}

void OnlinePanel::drawLogin(OnlineModel& online)
{
    ImGui::SeparatorText("Test login");
    beginCard();
    if (online.sessionLent()) {
        ImGui::TextWrapped("The Play session holds the login until Stop");
    } else if (online.loggedIn()) {
        emailLoaded_ = false; // after Log out, the field shows the email used
        const nlohmann::json account = online.account();
        ImGui::Text("Logged in as %s (%s)", account.value("email", "").c_str(), account.value("role", "").c_str());
        if (ImGui::Button("Log out"))
            online.logout();
        ImGui::TextDisabled("Play starts logged in as this account (auth.session)");
    } else {
        if (!emailLoaded_) {
            emailDraft_ = online.email();
            emailLoaded_ = true;
        }
        ImGui::InputText("Email", &emailDraft_);
        ImGui::InputText("Password", &password_, ImGuiInputTextFlags_Password);
        ImGui::BeginDisabled(online.loggingIn() || emailDraft_.empty() || password_.empty());
        if (ImGui::Button(online.loggingIn() ? "Logging in..." : "Log in"))
            online.login(emailDraft_, password_);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Kept in memory for this editor session only; the password is never stored");
        errorText(online.loginError());
    }
    endCard();
}

void OnlinePanel::drawWorldOverride(OnlineModel& online, const std::string& worldAddressFlag)
{
    ImGui::SeparatorText("World server override (Editor Play only)");
    beginCard();
    if (!overrideLoaded_) {
        overrideDraft_ = online.worldOverride();
        overrideLoaded_ = true;
    }
    ImGui::BeginDisabled(!worldAddressFlag.empty());
    ImGui::InputTextWithHint("host:port", "e.g. 127.0.0.1:7777, empty = the realm's own", &overrideDraft_);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        overrideError_.clear();
        if (!online.setWorldOverride(overrideDraft_, overrideError_))
            overrideDraft_ = online.worldOverride();
    }
    ImGui::EndDisabled();
    if (!worldAddressFlag.empty())
        reason(std::format("--world-address {} is in effect for this run", worldAddressFlag));
    ImGui::TextDisabled("Every realm connects here in Editor Play; built games ignore it");
    if (!overrideError_.empty())
        ImGui::TextColored(kErrorColor, "%s", overrideError_.c_str());
    endCard();
}

void OnlinePanel::drawAdmin(OnlineModel& online)
{
    ImGui::SeparatorText("Admin");
    beginCard();
    const OnlineModel::AdminResult& result = online.adminResult();
    ImGui::BeginDisabled(result.busy || online.sessionLent());

    ImGui::TextUnformatted("Realms");
    if (ImGui::BeginTable("adminRealms", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const OnlineRealm& realm : online.realms()) {
            ImGui::PushID(static_cast<int>(realm.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%lld %s", static_cast<long long>(realm.id), realm.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s:%u", realm.host.c_str(), realm.port);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Edit"))
                realm_ = {true, realm.id, realm.name, realm.host, realm.region, realm.port, realm.sortOrder,
                          indexOf(kRealmStatuses, static_cast<int>(std::size(kRealmStatuses)), realm.status)};
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete"))
                confirm(std::format("Delete realm '{}' (id {})?", realm.name, realm.id),
                        [&online, id = realm.id] { online.deleteRealm(id); });
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!realm_.open && ImGui::Button("New realm..."))
        realm_ = {.open = true};
    if (realm_.open)
        drawRealmForm(online);

    ImGui::Separator();
    ImGui::TextUnformatted("Account");
    ImGui::InputScalar("Account id", ImGuiDataType_S64, &accountId_);
    ImGui::InputText("Ban reason", &banReason_);
    ImGui::InputTextWithHint("Ban expires", "ISO 8601, e.g. 2030-01-01T00:00:00Z; empty = permanent", &banExpires_);
    ImGui::BeginDisabled(accountId_ <= 0 || banReason_.empty());
    if (ImGui::Button("Ban"))
        confirm(std::format("Ban account {} ({})?", accountId_, banExpires_.empty() ? "permanently" : "until " + banExpires_),
                [&online, id = accountId_, why = banReason_, until = banExpires_] { online.ban(id, why, until); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(accountId_ <= 0);
    if (ImGui::Button("Unban"))
        online.unban(accountId_);
    ImGui::Combo("Role", &role_, kRoles, static_cast<int>(std::size(kRoles)));
    if (ImGui::Button("Set role"))
        confirm(std::format("Make account {} a{} {}?", accountId_, role_ == 2 ? "n" : "", kRoles[role_]),
                [&online, id = accountId_, role = std::string(kRoles[role_])] { online.setRole(id, role); });
    ImGui::EndDisabled();
    reason("Account ids come from the auth database: the service has no account search");

    ImGui::EndDisabled();
    if (result.busy)
        ImGui::TextDisabled("%s...", result.op.c_str());
    else if (result.ok)
        ImGui::TextColored(kOkColor, "%s done", result.op.c_str());
    errorText(result.error);
    endCard();
}

void OnlinePanel::drawRealmForm(OnlineModel& online)
{
    ImGui::PushID("realmForm");
    ImGui::TextUnformatted(realm_.id ? std::format("Edit realm {}", realm_.id).c_str() : "New realm");
    ImGui::InputText("Name", &realm_.name);
    ImGui::InputText("Host", &realm_.host);
    ImGui::InputInt("Port", &realm_.port);
    ImGui::InputText("Region", &realm_.region);
    ImGui::Combo("Status", &realm_.status, kRealmStatuses, static_cast<int>(std::size(kRealmStatuses)));
    ImGui::InputInt("Sort order", &realm_.sortOrder);
    if (ImGui::Button("Save")) {
        const nlohmann::json input{{"name", realm_.name}, {"host", realm_.host},    {"port", realm_.port},
                                   {"region", realm_.region}, {"status", kRealmStatuses[realm_.status]},
                                   {"sortOrder", realm_.sortOrder}};
        if (realm_.id)
            online.updateRealm(realm_.id, input);
        else
            online.createRealm(input);
        realm_.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        realm_.open = false;
    ImGui::PopID();
}

void OnlinePanel::confirm(std::string text, std::function<void()> action)
{
    confirmText_ = std::move(text);
    confirmAction_ = std::move(action);
    confirmRequested_ = true;
}

void OnlinePanel::drawConfirm()
{
    if (confirmRequested_) {
        ImGui::OpenPopup("Confirm##online");
        confirmRequested_ = false;
    }
    if (!ImGui::BeginPopupModal("Confirm##online", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted(confirmText_.c_str());
    if (ImGui::Button("Yes")) {
        if (confirmAction_)
            confirmAction_();
        confirmAction_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        confirmAction_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
