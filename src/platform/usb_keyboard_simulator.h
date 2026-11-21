#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

using KeyboardReport = std::array<uint8_t, 8>;

bool make_simulated_keyboard_reports(const std::string& text,
                                     std::vector<KeyboardReport>& reports,
                                     std::string& error);

} // namespace platform
