#include "char_repo.h"
#include "../core/utc.h"

#include <string>

namespace kast::db {
namespace {

std::string now_utc() { return utc_mysql(std::chrono::system_clock::now()); }

CharRow to_row(const Row& r) {
    CharRow c;
    c.id = std::stoull(*r[0]);
    c.accountId = std::stoull(*r[1]);
    c.name = *r[2];
    c.pos = {std::stof(*r[3]), std::stof(*r[4]), std::stof(*r[5])};
    c.heading = std::stof(*r[6]);
    return c;
}

constexpr const char* kColumns = "id, account_id, name, pos_x, pos_y, pos_z, heading";

} // namespace

std::vector<CharRow> CharRepo::list(uint64_t accountId) {
    auto res = c_.exec(std::string("SELECT ") + kColumns +
                           " FROM characters WHERE realm_id = ? AND account_id = ? AND deleted_at IS NULL ORDER BY id",
                       {uint64_t{realm_}, accountId});
    std::vector<CharRow> out;
    for (const auto& r : res.rows) out.push_back(to_row(r));
    return out;
}

CreateResult CharRepo::create(uint64_t accountId, const std::string& name, proto::Vec3 spawn, float heading,
                              uint32_t maxChars) {
    CreateResult result;
    auto count = c_.exec("SELECT COUNT(*) FROM characters WHERE realm_id = ? AND account_id = ? AND deleted_at IS NULL",
                         {uint64_t{realm_}, accountId});
    if (std::stoull(*count.rows[0][0]) >= maxChars) { result.code = proto::Code::char_limit; return result; }
    try {
        auto ins = c_.exec("INSERT INTO characters (realm_id, account_id, name, pos_x, pos_y, pos_z, heading, created_at) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                           {uint64_t{realm_}, accountId, name, double{spawn.x}, double{spawn.y}, double{spawn.z},
                            double{heading}, now_utc()});
        result.row = {ins.insertId, accountId, name, spawn, heading};
    } catch (const DbError& e) {
        if (e.code != kErDupEntry) throw;
        result.code = proto::Code::name_taken;
    }
    return result;
}

bool CharRepo::soft_delete(uint64_t accountId, uint64_t charId) {
    auto res = c_.exec("UPDATE characters SET deleted_at = ? WHERE id = ? AND realm_id = ? AND account_id = ? "
                       "AND deleted_at IS NULL",
                       {now_utc(), charId, uint64_t{realm_}, accountId});
    return res.affectedRows == 1;
}

std::optional<CharRow> CharRepo::enter(uint64_t accountId, uint64_t charId) {
    auto res = c_.exec(std::string("SELECT ") + kColumns +
                           " FROM characters WHERE id = ? AND realm_id = ? AND account_id = ? AND deleted_at IS NULL",
                       {charId, uint64_t{realm_}, accountId});
    if (res.rows.empty()) return std::nullopt;
    c_.exec("UPDATE characters SET last_login_at = ? WHERE id = ?", {now_utc(), charId});
    return to_row(res.rows[0]);
}

void CharRepo::save(const CharSave& s) {
    c_.exec("UPDATE characters SET pos_x = ?, pos_y = ?, pos_z = ?, heading = ? WHERE id = ? AND realm_id = ?",
            {double{s.pos.x}, double{s.pos.y}, double{s.pos.z}, double{s.heading}, s.charId, uint64_t{realm_}});
}

} // namespace kast::db
