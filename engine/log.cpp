#include "log.h"

#include <cstdio>
#include <format>

namespace kast::log {
namespace {

// ponytail: unbounded, single-threaded; cap it / add a mutex when logging gets heavy or goes multi-threaded.
std::vector<std::string> g_lines;

void write(std::string_view level, std::string_view message)
{
    std::string line = std::format("[{}] {}", level, message);
    std::puts(line.c_str());
    std::fflush(stdout);
    g_lines.push_back(std::move(line));
}

} // namespace

void info(std::string_view message) { write("info", message); }
void warn(std::string_view message) { write("warn", message); }
void error(std::string_view message) { write("error", message); }

const std::vector<std::string>& lines() { return g_lines; }

} // namespace kast::log
