#pragma once

#include <cstdint>
#include <string>

namespace platform {

double calculate_expression(const std::string& expression, bool& valid);
std::string encode_morse(const std::string& text);
std::string decode_morse(const std::string& morse);
std::string current_clock_text();
std::string local_identity_text();
std::int64_t uptime_seconds();

} // namespace platform
