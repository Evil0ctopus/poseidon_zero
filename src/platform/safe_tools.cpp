#include "safe_tools.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

namespace platform {
namespace {

class ExpressionParser {
public:
    explicit ExpressionParser(const std::string& text) : text_(text) {}

    double parse(bool& valid) {
        position_ = 0;
        valid = true;
        const double value = expression(valid);
        while (valid && position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        if (position_ != text_.size()) {
            valid = false;
        }
        return valid ? value : 0.0;
    }

private:
    double expression(bool& valid) {
        double value = term(valid);
        while (valid) {
            skip_space();
            if (position_ >= text_.size()) {
                break;
            }
            const char op = text_[position_];
            if (op != '+' && op != '-') {
                break;
            }
            ++position_;
            const double right = term(valid);
            value = op == '+' ? value + right : value - right;
        }
        return value;
    }

    double term(bool& valid) {
        double value = factor(valid);
        while (valid) {
            skip_space();
            if (position_ >= text_.size()) {
                break;
            }
            const char op = text_[position_];
            if (op != '*' && op != '/') {
                break;
            }
            ++position_;
            const double right = factor(valid);
            if (op == '/' && right == 0.0) {
                valid = false;
                return 0.0;
            }
            value = op == '*' ? value * right : value / right;
        }
        return value;
    }

    double factor(bool& valid) {
        skip_space();
        if (position_ >= text_.size()) {
            valid = false;
            return 0.0;
        }
        if (text_[position_] == '(') {
            ++position_;
            const double value = expression(valid);
            skip_space();
            if (!valid || position_ >= text_.size() || text_[position_] != ')') {
                valid = false;
                return 0.0;
            }
            ++position_;
            return value;
        }
        if (!std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            valid = false;
            return 0.0;
        }
        std::size_t end = position_;
        while (end < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[end])) || text_[end] == '.')) {
            ++end;
        }
        const std::string number = text_.substr(position_, end - position_);
        position_ = end;
        char* stop = nullptr;
        const double value = std::strtod(number.c_str(), &stop);
        if (!stop || stop == number.c_str()) {
            valid = false;
        }
        return value;
    }

    void skip_space() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
    }

    const std::string& text_;
    std::size_t position_{0};
};

} // namespace

double calculate_expression(const std::string& expression, bool& valid) {
    ExpressionParser parser(expression);
    return parser.parse(valid);
}

std::string encode_morse(const std::string& text) {
    static const char* codes[] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
        "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
        "..-", "...-", ".--", "-..-", "-.--", "--.."
    };
    std::string result;
    for (char character : text) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        if (character >= 'A' && character <= 'Z') {
            if (!result.empty()) {
                result += ' ';
            }
            result += codes[character - 'A'];
        } else if (character == ' ') {
            if (!result.empty() && result.back() != '/') {
                result += " / ";
            }
        }
    }
    return result;
}

std::string decode_morse(const std::string& morse) {
    static const char* codes[] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
        "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
        "..-", "...-", ".--", "-..-", "-.--", "--.."
    };
    std::string result;
    std::size_t start = 0;
    while (start <= morse.size()) {
        const std::size_t end = morse.find(' ', start);
        const std::string token = morse.substr(start, end == std::string::npos ? morse.size() - start : end - start);
        if (token == "/" || token.empty()) {
            if (!result.empty() && result.back() != ' ') {
                result += ' ';
            }
        } else {
            for (char letter = 'A'; letter <= 'Z'; ++letter) {
                if (token == codes[letter - 'A']) {
                    result += letter;
                    break;
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::string current_clock_text() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return text;
}

std::int64_t uptime_seconds() {
    std::ifstream uptime("/proc/uptime");
    double seconds = 0.0;
    if (uptime >> seconds) {
        return static_cast<std::int64_t>(seconds);
    }
    return 0;
}

} // namespace platform
