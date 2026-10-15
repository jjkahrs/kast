// Structured JSON-line logging to stdout. Thread-safe. Never pass tokens, passwords or chat text at info level.
#pragma once
#include <nlohmann/json.hpp>

#include <string_view>

namespace kast::log {

enum class Level { debug = 0, info = 1, warn = 2, error = 3 };

void set_level(Level l);
bool parse_level(std::string_view s, Level& out);
void write(Level l, std::string_view msg, nlohmann::json fields = nlohmann::json::object());

inline void debug(std::string_view m, nlohmann::json f = nlohmann::json::object()) { write(Level::debug, m, std::move(f)); }
inline void info(std::string_view m, nlohmann::json f = nlohmann::json::object()) { write(Level::info, m, std::move(f)); }
inline void warn(std::string_view m, nlohmann::json f = nlohmann::json::object()) { write(Level::warn, m, std::move(f)); }
inline void error(std::string_view m, nlohmann::json f = nlohmann::json::object()) { write(Level::error, m, std::move(f)); }

} // namespace kast::log
