#pragma once

#include "document.h"
#include "online.h"

#include <cstdint>
#include <functional>
#include <string>

// Window > Online: the ImGui view of an OnlineModel (online.h) for the open project: status, local stack, test login,
// Editor Play's world override and, for an admin login, realm / ban / role management with confirmations.
class OnlinePanel {
public:
    // Draws nothing while open is false. worldAddressFlag: --world-address, which wins over the override field.
    void draw(const Document& doc, OnlineModel& online, FileActions& files, const std::string& worldAddressFlag,
              bool& open);
    // Whether the window's content was drawn last frame (open, not collapsed, its tab in front).
    bool visible() const { return visible_; }

private:
    void drawStatus(OnlineModel& online);
    void drawStack(OnlineModel& online);
    void drawLogin(OnlineModel& online);
    void drawWorldOverride(OnlineModel& online, const std::string& worldAddressFlag);
    void drawAdmin(OnlineModel& online);
    void drawRealmForm(OnlineModel& online);
    void confirm(std::string text, std::function<void()> action); // asks first, runs action on Yes
    void drawConfirm();

    bool visible_ = false;
    std::string password_; // only until Log in is pressed (the model wipes it)
    std::string emailDraft_;
    bool emailLoaded_ = false;
    std::string overrideDraft_, overrideError_;
    bool overrideLoaded_ = false;

    struct RealmForm {
        bool open = false;
        int64_t id = 0; // 0 = a new realm
        std::string name, host, region;
        int port = 7777, sortOrder = 0, status = 0; // status: index in kRealmStatuses
    } realm_;
    int64_t accountId_ = 0;
    std::string banReason_, banExpires_;
    int role_ = 0; // index in kRoles

    std::string confirmText_;
    std::function<void()> confirmAction_;
    bool confirmRequested_ = false;
};
