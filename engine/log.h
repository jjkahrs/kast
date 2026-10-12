#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace kast::log {

void info(std::string_view message);
void warn(std::string_view message);
void error(std::string_view message);

// Every line logged so far, oldest first. Main thread only.
const std::vector<std::string>& lines();

} // namespace kast::log
