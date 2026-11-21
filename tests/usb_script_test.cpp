#include "usb_script.h"

#include <iostream>

int main() {
    std::vector<platform::UsbScriptStep> steps;
    std::string error;
    const std::string script =
        "REM lab test\nDELAY 500\nGUI r\nSTRING hi\nENTER\nCTRL ALT DELETE\n";
    if (!platform::parse_usb_script(script, steps, error) || steps.size() != 1 + 2 + 4 + 2 + 2) {
        std::cerr << "Valid script failed: " << error << "\n";
        return 1;
    }
    if (steps[0].delay_ms != 500 || steps[1].report[0] != 0x08 || steps[1].report[2] != 21 ||
        steps[3].report[2] != 11 || steps[7].report[2] != 40 ||
        steps[9].report[0] != 0x05 || steps[9].report[2] != 76) {
        std::cerr << "Incorrect parsed steps\n";
        return 1;
    }
    for (const char* bad : {"", "BOGUS", "DELAY -1", "STRING", "GUI bogus"}) {
        if (platform::parse_usb_script(bad, steps, error) || !steps.empty() || error.empty()) {
            std::cerr << "Invalid script must fail: " << bad << "\n";
            return 1;
        }
    }
    return 0;
}
