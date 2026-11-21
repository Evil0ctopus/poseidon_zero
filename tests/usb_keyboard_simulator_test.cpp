#include "usb_keyboard_simulator.h"

#include <iostream>

int main() {
    std::vector<platform::KeyboardReport> reports;
    std::string error;
    if (!platform::make_simulated_keyboard_reports("aA1! ;:?", reports, error) ||
        reports.size() != 16) {
        std::cerr << "Expected press and release for each character\n";
        return 1;
    }
    const uint8_t expected[] = {4, 4, 30, 30, 44, 51, 51, 56};
    const uint8_t modifiers[] = {0, 2, 0, 2, 0, 0, 2, 2};
    for (std::size_t i = 0; i < 8; ++i) {
        if (reports[i * 2][0] != modifiers[i] ||
            reports[i * 2][2] != expected[i] ||
            reports[i * 2 + 1] != platform::KeyboardReport{}) {
            std::cerr << "Incorrect simulated key or release report\n";
            return 1;
        }
    }
    for (const auto& invalid : {std::string{}, std::string(65, 'a'), std::string("a\n")}) {
        if (platform::make_simulated_keyboard_reports(invalid, reports, error) ||
            !reports.empty() || error.empty()) {
            std::cerr << "Invalid input must fail without partial reports\n";
            return 1;
        }
    }
    if (!platform::make_simulated_keyboard_reports(std::string(64, 'z'), reports, error) ||
        reports.size() != 128 || reports.front()[2] != 29 || !error.empty()) {
        std::cerr << "A 64-character input must produce complete reports\n";
        return 1;
    }
    return 0;
}
