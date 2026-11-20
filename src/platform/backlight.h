#pragma once

#include <cstdint>
#include <string>

namespace platform {

bool read_brightness_percent(int32_t& percent, std::string& error);
bool write_brightness_percent(int32_t percent, std::string& error);

} // namespace platform
