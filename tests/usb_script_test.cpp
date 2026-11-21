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

    if (!platform::parse_usb_script("GUI !\nGUI A\nF1\nF12\n", steps, error) ||
        steps.size() != 8 || steps[0].report[0] != 0x0a ||
        steps[0].report[2] != 30 || steps[2].report[0] != 0x0a ||
        steps[2].report[2] != 4 || steps[4].report[2] != 58 ||
        steps[6].report[2] != 69) {
        std::cerr << "Character modifiers or function keys failed: " << error << "\n";
        return 1;
    }

    if (!platform::parse_usb_script("SHIFT @\nCTRL ALT Z\n", steps, error) ||
        steps.size() != 4 || steps[0].report[0] != 0x02 ||
        steps[0].report[2] != 31 || steps[2].report[0] != 0x07 ||
        steps[2].report[2] != 29) {
        std::cerr << "Shifted punctuation or uppercase character failed: " << error << "\n";
        return 1;
    }

    if (!platform::parse_usb_script("STRING   hi\nSTRING   \nDELAY 500   \n", steps, error) ||
        steps.size() != 13 ||
        steps[0].report[2] != 44 || steps[2].report[2] != 44 ||
        steps[8].report[2] != 44 || steps[12].delay_ms != 500) {
        std::cerr << "STRING or DELAY whitespace handling failed: " << error << "\n";
        return 1;
    }

    for (const char* bad : {"", "BOGUS", "DELAY -1", "DELAY 60001",
                            "DELAY 1.5", "DELAY 1oops", "DELAY 500 ENTER",
                            "F1garbage", "F12.5", "F01", "F0", "F13", "STRING",
                            "GUI bogus"}) {
        if (platform::parse_usb_script(bad, steps, error) || !steps.empty() || error.empty()) {
            std::cerr << "Invalid script must fail: " << bad << "\n";
            return 1;
        }
    }
    return 0;
}
