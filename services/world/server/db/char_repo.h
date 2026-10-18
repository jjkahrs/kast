// Character persistence (DB kast_world). Every query is scoped by realm_id; ownership checks use account_id.
// Runs on the DB worker thread only.
#pragma once
#include "mysql.h"

#include <messages.h>

#include <optional>
#include <vector>

namespace kast::db {

struct CharRow {
    uint64_t id = 0;
    uint64_t accountId = 0;
    std::string name;
    uint16_t mapId = 0;
    proto::Vec3 pos;
    float heading = 0;
    std::string components; // the components JSON column (Lua components, see lua/components.h); empty if NULL
    proto::CharOptions options = {}; // the options JSON column (CharCreate options); empty if NULL
};

struct CharSave {
    uint64_t charId = 0;
    uint16_t mapId = 0;
    proto::Vec3 pos;
    float heading = 0;
    std::optional<std::string> components; // nullopt: leave the column as it is
};

struct CreateResult {
    proto::Code code = proto::Code::ok; // ok, char_limit or name_taken
    CharRow row;
};

class CharRepo {
public:
    CharRepo(Connection& c, uint32_t realmId) : c_(c), realm_(realmId) {}

    std::vector<CharRow> list(uint64_t accountId);
    // ponytail: count-then-insert is race-free only with one writer per realm (this server's single DB thread);
    // several world processes per realm would need SELECT ... FOR UPDATE in a transaction.
    CreateResult create(uint64_t accountId, const std::string& name, const proto::CharOptions& options, uint16_t mapId,
                        proto::Vec3 spawn, float heading, uint32_t maxChars);
    bool soft_delete(uint64_t accountId, uint64_t charId); // false = not found / not owned / already deleted
    std::optional<CharRow> enter(uint64_t accountId, uint64_t charId); // also stamps last_login_at
    void save(const CharSave& s);

private:
    Connection& c_;
    uint32_t realm_;
};

} // namespace kast::db
