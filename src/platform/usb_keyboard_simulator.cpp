#include "usb_keyboard_simulator.h"

#include <string_view>

namespace platform {

bool make_simulated_keyboard_reports(const std::string& text,
                                     std::vector<KeyboardReport>& reports,
                                     std::string& error) {
    reports.clear();
    error.clear();
    if (text.empty() || text.size() > 64) {
        error = "Enter 1 to 64 characters";
        return false;
    }

    constexpr std::string_view plain = "1234567890-=[]\\;'`,./";
    constexpr std::string_view shifted = "!@#$%^&*()_+{}|:\"~<>?";
    for (const unsigned char character : text) {
        uint8_t usage = 0;
        bool shift = false;
        if (character >= 'a' && character <= 'z') {
            usage = static_cast<uint8_t>(4 + character - 'a');
        } else if (character >= 'A' && character <= 'Z') {
            usage = static_cast<uint8_t>(4 + character - 'A');
            shift = true;
        } else if (character == ' ') {
            usage = 44;
        } else {
            auto position = plain.find(static_cast<char>(character));
            if (position == std::string_view::npos) {
                position = shifted.find(static_cast<char>(character));
                shift = true;
            }
            if (position != std::string_view::npos) {
                constexpr uint8_t usages[] = {
                    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
                    45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56
                };
                usage = usages[position];
            }
        }
        if (!usage) {
            reports.clear();
            error = "Unsupported character; use printable US keyboard characters";
            return false;
        }
        KeyboardReport press{};
        press[0] = shift ? 0x02 : 0;
        press[2] = usage;
        reports.push_back(press);
        reports.emplace_back();
    }
    return true;
}

} // namespace platform
