#include "safe_tools.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

class ExpressionParser {
public:
    explicit ExpressionParser(const std::string& text) : text_(text) {}

    double parse(bool& valid) {
        position_ = 0;
        valid = true;
        if (text_.empty() || text_.size() > 64) {
            valid = false;
            return 0.0;
        }
        const double value = expression(valid);
        while (valid && position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        if (position_ != text_.size() || !std::isfinite(value)) {
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
        if (text_[position_] == '+' || text_[position_] == '-') {
            const char sign = text_[position_++];
            const double value = factor(valid);
            return sign == '-' ? -value : value;
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

        std::size_t end = position_;
        bool has_digit = false;
        while (end < text_.size() && std::isdigit(static_cast<unsigned char>(text_[end]))) {
            has_digit = true;
            ++end;
        }
        if (end < text_.size() && text_[end] == '.') {
            ++end;
            while (end < text_.size() && std::isdigit(static_cast<unsigned char>(text_[end]))) {
                has_digit = true;
                ++end;
            }
        }
        if (!has_digit) {
            valid = false;
            return 0.0;
        }

        const std::string number = text_.substr(position_, end - position_);
        position_ = end;
        char* stop = nullptr;
        const double value = std::strtod(number.c_str(), &stop);
        if (!stop || stop != number.c_str() + number.size()) {
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

std::string format_clock_text(const std::tm& local_time,
                              bool clock_24h,
                              bool include_seconds) {
    const char* format = clock_24h
                             ? (include_seconds ? "%H:%M:%S" : "%H:%M")
                             : (include_seconds ? "%I:%M:%S %p" : "%I:%M %p");
    char text[32]{};
    return std::strftime(text, sizeof(text), format, &local_time) == 0 ? "" : text;
}

std::string current_clock_text(bool clock_24h, bool include_seconds) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return format_clock_text(local, clock_24h, include_seconds);
}

std::string local_identity_text() {
#if defined(_WIN32)
    const char* user_name = std::getenv("USERNAME");
    const char* host_name = std::getenv("COMPUTERNAME");
    const std::string user = user_name && user_name[0] != '\0' ? user_name : "unavailable";
    const std::string host = host_name && host_name[0] != '\0' ? host_name : "unavailable";
#else
    const uid_t user_id = geteuid();
    const passwd* account = getpwuid(user_id);
    const std::string user =
        account && account->pw_name ? std::string(account->pw_name)
                                    : "uid " + std::to_string(user_id);

    char host_name[256]{};
    const std::string host =
        gethostname(host_name, sizeof(host_name)) == 0 && host_name[0] != '\0'
            ? host_name
            : "unavailable";
#endif
    return "User " + user + "\nHost " + host;
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
