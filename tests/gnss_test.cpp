#include "gnss.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

namespace {

std::string sentence(const std::string& body) {
    constexpr char hex[] = "0123456789ABCDEF";
    unsigned checksum = 0;
    for (unsigned char c : body) checksum ^= c;
    return "$" + body + "*" + hex[checksum >> 4] + hex[checksum & 15] + "\r\n";
}

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main() {
    using namespace platform;
    GnssFix fix;
    bool ok = true;
    const auto valid = sentence("GNRMC,123519.250,A,3853.8000,N,09217.4000,W,0,0,240526,,,A");
    ok &= check(parse_gnss_sentence(valid, fix) == GnssSentenceStatus::Fix, "Valid RMC rejected");
    ok &= check(std::abs(fix.latitude - 38.8966666667) < 1e-8 &&
                std::abs(fix.longitude + 92.29) < 1e-8, "Incorrect coordinates");
    ok &= check(fix.mode == "A", "Incorrect RMC fix mode");
    const auto expected = std::chrono::system_clock::time_point{} +
                          std::chrono::seconds(1779626119) + std::chrono::milliseconds(250);
    ok &= check(fix.utc == expected, "Incorrect UTC timestamp");
    ok &= check(gnss_fix_is_fresh(fix, expected + std::chrono::seconds(5)), "Fresh fix rejected");
    ok &= check(!gnss_fix_is_fresh(fix, expected + std::chrono::seconds(6)), "Stale fix accepted");
    ok &= check(!gnss_fix_is_fresh(fix, expected - std::chrono::seconds(3)), "Future fix accepted");
    auto corrupted = valid;
    corrupted[10] = corrupted[10] == '1' ? '2' : '1';
    ok &= check(parse_gnss_sentence(corrupted, fix) == GnssSentenceStatus::Invalid, "Bad checksum accepted");
    ok &= check(parse_gnss_sentence(sentence("GPRMC,123519,V,,,,,,,240526"), fix) ==
                GnssSentenceStatus::NoFix, "No-fix status rejected");
    ok &= check(parse_gnss_sentence(sentence("GNRMC,123519,A,9000.01,N,09217.4,W,0,0,240526"), fix) ==
                GnssSentenceStatus::Invalid, "Out-of-range coordinate accepted");
    ok &= check(parse_gnss_sentence(sentence("GNRMC,123519,A,3853.8,N,09217.4,W,0,0,310226"), fix) ==
                GnssSentenceStatus::Invalid, "Invalid date accepted");
    ok &= check(parse_gnss_sentence(sentence("GNRMC,123519,A,3853.8,N,09217.4,W,0,0,240526,,,N"), fix) ==
                GnssSentenceStatus::NoFix, "Invalid RMC mode accepted");
    ok &= check(parse_gnss_sentence(sentence("GNRMC,123519,A,3853.8,N,09217.4,W,0,0,240526,,,S"), fix) ==
                GnssSentenceStatus::NoFix, "Simulated RMC position accepted");
    ok &= check(parse_gnss_sentence(sentence("GNGGA,123519"), fix) ==
                GnssSentenceStatus::Other, "Other sentence misclassified");
    return ok ? 0 : 1;
}
