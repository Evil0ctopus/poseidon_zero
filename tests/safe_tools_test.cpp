#include "safe_tools.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

bool expect_result(const std::string& expression, double expected) {
    bool valid = false;
    const double result = platform::calculate_expression(expression, valid);
    if (!valid || std::abs(result - expected) > 1e-9) {
        std::cerr << "Expected " << expression << " to equal " << expected
                  << ", got " << result << " (valid=" << valid << ")\n";
        return false;
    }
    return true;
}

bool expect_invalid(const std::string& expression) {
    bool valid = true;
    platform::calculate_expression(expression, valid);
    if (valid) {
        std::cerr << "Expected invalid expression: " << expression << '\n';
        return false;
    }
    return true;
}

bool expect_string(const std::string& actual,
                   const std::string& expected,
                   const std::string& description) {
    if (actual != expected) {
        std::cerr << "Expected " << description << " to be \"" << expected
                  << "\", got \"" << actual << "\"\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    bool passed = true;
    passed &= expect_result("1+2*3", 7.0);
    passed &= expect_result("-(2 + 3) * .5", -2.5);
    passed &= expect_result("2--3", 5.0);
    passed &= expect_invalid("");
    passed &= expect_invalid("1..2");
    passed &= expect_invalid("1/(2-2)");
    passed &= expect_invalid(std::string(65, '1'));
    passed &= expect_string(platform::encode_morse("SOS"),
                            "... --- ...",
                            "Morse encoding");
    passed &= expect_string(platform::decode_morse("... --- ..."),
                            "SOS",
                            "Morse decoding");
    return passed ? 0 : 1;
}
