#include "usb_script.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace platform {
namespace {

constexpr std::size_t kMaxSteps = 4096;
constexpr uint32_t kMaxDelayMs = 60000;

struct Named {
    const char* name;
    uint8_t value;
};

constexpr Named kModifiers[] = {
    {"CTRL", 0x01}, {"CONTROL", 0x01}, {"SHIFT", 0x02}, {"ALT", 0x04},
    {"GUI", 0x08}, {"WINDOWS", 0x08}, {"COMMAND", 0x08},
};

constexpr Named kKeys[] = {
    {"ENTER", 40}, {"ESC", 41}, {"ESCAPE", 41}, {"BACKSPACE", 42}, {"TAB", 43},
    {"SPACE", 44}, {"DELETE", 76}, {"RIGHT", 79}, {"RIGHTARROW", 79},
    {"LEFT", 80}, {"LEFTARROW", 80}, {"DOWN", 81}, {"DOWNARROW", 81},
    {"UP", 82}, {"UPARROW", 82}, {"HOME", 74}, {"END", 77},
    {"PAGEUP", 75}, {"PAGEDOWN", 78},
};

template <std::size_t N>
const Named* find_named(const Named (&table)[N], const std::string& name) {
    for (const auto& entry : table) {
        if (name == entry.name) return &entry;
    }
    return nullptr;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

bool key_usage(const std::string& token, uint8_t& usage, uint8_t& modifiers) {
    modifiers = 0;
    if (const auto* key = find_named(kKeys, upper(token))) {
        usage = key->value;
        return true;
    }
    if (token.size() == 1) {
        std::vector<KeyboardReport> reports;
        std::string ignored;
        if (make_simulated_keyboard_reports(token, reports, ignored)) {
            modifiers = reports.front()[0];
            usage = reports.front()[2];
            return true;
        }
    }
    const std::string name = upper(token);
    if (name.size() == 2 || name.size() == 3) {
        if (name[0] == 'F') {
            int number = 0;
            const char* first = name.data() + 1;
            const char* last = name.data() + name.size();
            const auto result = std::from_chars(first, last, number);
            if (result.ec == std::errc{} && result.ptr == last &&
                number >= 1 && number <= 12) {
                usage = static_cast<uint8_t>(57 + number);
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool parse_usb_script(const std::string& script,
                      std::vector<UsbScriptStep>& steps,
                      std::string& error) {
    steps.clear();
    error.clear();
    std::istringstream input(script);
    std::string line;
    int line_number = 0;

    auto fail = [&](const std::string& message) {
        steps.clear();
        error = "Line " + std::to_string(line_number) + ": " + message;
        return false;
    };

    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line.erase(0, first);

        std::istringstream words(line);
        std::string command;
        words >> command;
        const std::string upper_command = upper(command);
        if (upper_command == "REM") continue;

        if (upper_command == "DELAY") {
            std::string remainder = line.substr(command.size());
            const auto first = remainder.find_first_not_of(" \t");
            if (first == std::string::npos) {
                return fail("DELAY needs 0 to 60000 ms");
            }
            remainder.erase(0, first);
            const auto argument_end = remainder.find_first_of(" \t");
            const std::string argument =
                argument_end == std::string::npos ? remainder : remainder.substr(0, argument_end);
            if (argument_end != std::string::npos &&
                remainder.find_first_not_of(" \t", argument_end) != std::string::npos) {
                return fail("DELAY needs 0 to 60000 ms");
            }
            int value = 0;
            const auto result = std::from_chars(argument.data(),
                                                argument.data() + argument.size(), value);
            if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() ||
                value < 0 || static_cast<uint32_t>(value) > kMaxDelayMs) {
                return fail("DELAY needs 0 to 60000 ms");
            }
            UsbScriptStep step;
            step.delay_ms = static_cast<uint32_t>(value);
            steps.push_back(step);
        } else if (upper_command == "STRING") {
            if (line.size() <= command.size() ||
                (line[command.size()] != ' ' && line[command.size()] != '\t')) {
                return fail("STRING needs text");
            }
            const std::size_t text_start = command.size() + 1;
            std::vector<KeyboardReport> reports;
            std::string text_error;
            if (text_start > line.size() ||
                !make_simulated_keyboard_reports(line.substr(text_start), reports, text_error)) {
                return fail(text_start > line.size() ? "STRING needs text" : text_error);
            }
            for (const auto& report : reports) {
                UsbScriptStep step;
                step.report = report;
                steps.push_back(step);
            }
        } else {
            uint8_t modifiers = 0;
            uint8_t character_modifiers = 0;
            uint8_t usage = 0;
            bool have_key = false;
            std::string token = command;
            do {
                const auto* modifier = find_named(kModifiers, upper(token));
                if (modifier && !have_key) {
                    modifiers |= modifier->value;
                } else if (!have_key && key_usage(token, usage, character_modifiers)) {
                    have_key = true;
                } else {
                    return fail("Unknown command or key: " + token);
                }
            } while (words >> token);
            UsbScriptStep press;
            press.report[0] = modifiers | character_modifiers;
            press.report[2] = usage;
            steps.push_back(press);
            steps.emplace_back();
        }
        if (steps.size() > kMaxSteps) return fail("Script is too long");
    }
    if (steps.empty()) {
        error = "Script is empty";
        return false;
    }
    return true;
}

} // namespace platform
