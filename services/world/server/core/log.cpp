#include "log.h"
#include "utc.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace kast::log {
namespace {
std::atomic<Level> g_level{Level::info};
std::mutex g_mutex;
constexpr const char* kNames[] = {"debug", "info", "warn", "error"};
} // namespace

void set_level(Level l) { g_level = l; }

bool parse_level(std::string_view s, Level& out) {
    for (int i = 0; i < 4; ++i)
        if (s == kNames[i]) { out = static_cast<Level>(i); return true; }
    return false;
}

void write(Level l, std::string_view msg, nlohmann::json fields) {
    if (l < g_level.load()) return;
    nlohmann::json line = {{"ts", utc_iso8601(std::chrono::system_clock::now())},
                           {"level", kNames[static_cast<int>(l)]},
                           {"msg", msg}};
    if (fields.is_object()) line.update(fields);
    // replace: invalid UTF-8 from a client never throws inside logging
    const std::string out = line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
    std::lock_guard lock(g_mutex);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

} // namespace kast::log
