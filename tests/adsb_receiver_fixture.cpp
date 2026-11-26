#include <chrono>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <csignal>
#include <string>
#include <thread>

namespace {
void emit_frame(unsigned address) {
    std::array<uint8_t, 14> bytes{{0x8d, static_cast<uint8_t>(address >> 16),
        static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address),
        0x58, 0xc3, 0x82, 0xd6, 0x90, 0xc8, 0xac, 0, 0, 0}};
    uint32_t remainder = 0;
    for (const auto byte : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool top = (remainder & 0x800000U) != 0;
            remainder = ((remainder << 1) | ((byte >> bit) & 1U)) & 0xffffffU;
            if (top) remainder ^= 0xfff409U;
        }
    }
    bytes[11] = static_cast<uint8_t>(remainder >> 16);
    bytes[12] = static_cast<uint8_t>(remainder >> 8);
    bytes[13] = static_cast<uint8_t>(remainder);
    std::cout << '*';
    for (const auto byte : bytes)
        std::cout << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(byte);
    std::cout << ";\n";
}
}

int main() {
    const char* mode = std::getenv("POSEIDON_ADSB_TEST_MODE");
    if (mode && std::string(mode) == "tracks") {
        for (unsigned address = 1; address <= 33; ++address) emit_frame(address);
        emit_frame(2);
        std::cout << "invalid\n" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode && std::string(mode) == "exit") {
        std::cerr << "fixture unavailable\n";
        return 2;
    }
    if (mode && std::string(mode) == "flood") {
        for (int i = 0; i < 50000; ++i)
            std::cout << "*8D40621D58C382D690C8AC2863A7;\n";
        std::cout << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode && std::string(mode) == "gps") {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "*8D40621D58C382D690C8AC2863A7;\n" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode && (std::string(mode) == "emergency" || std::string(mode) == "category")) {
        std::cout << (std::string(mode) == "emergency"
            ? "*8DA2C1B6E12AAA000000000D81F4;\n"
            : "*8D4840D61E2CC371C32CE035A441;\n") << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode && std::string(mode) == "stubborn") std::signal(SIGTERM, SIG_IGN);
    std::cout << "*8D40621D58C3" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::cout << "82D690C8AC2863A7;\r\n"
                 "*8D40621D58C386435CC412692AD6;\n"
                 "*8D40621D58C382D690C8AC2863A6;\n" << std::flush;
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
