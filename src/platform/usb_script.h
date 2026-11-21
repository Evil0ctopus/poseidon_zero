#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usb_keyboard_simulator.h"

namespace platform {

struct UsbScriptStep {
    KeyboardReport report{};
    uint32_t delay_ms = 0;
};

// Parses a DuckyScript-style subset: REM, DELAY <ms>, STRING <text>, ENTER, TAB,
// ESC, SPACE, BACKSPACE, DELETE, arrow keys, and modifier combos such as
// "GUI r" or "CTRL ALT DELETE". Each key action emits a press and a release.
bool parse_usb_script(const std::string& script,
                      std::vector<UsbScriptStep>& steps,
                      std::string& error);

} // namespace platform
