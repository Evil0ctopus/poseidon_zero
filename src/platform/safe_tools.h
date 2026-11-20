#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace platform {

double calculate_expression(const std::string& expression, bool& valid);
std::string encode_morse(const std::string& text);
std::string decode_morse(const std::string& morse);
std::string format_clock_text(const std::tm& local_time,
                              bool clock_24h,
                              bool include_seconds = true);
std::string current_clock_text(bool clock_24h = true, bool include_seconds = true);
std::string local_identity_text();
std::int64_t uptime_seconds();

} // namespace platform
