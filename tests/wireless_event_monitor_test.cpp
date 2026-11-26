#include "wireless_event_monitor.h"
#include "esp32_monitor_serial.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

#if defined(POSEIDON_TEST_WIRELESS_EVENT) && !USE_DESKTOP
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#endif

int main() {
    std::array<uint8_t, 320> packet{};
    packet[2] = 8;
    packet[8] = 0xc0;
    for (unsigned i = 0; i < 6; ++i) packet[8 + 16 + i] = static_cast<uint8_t>(i + 1);
    packet[8 + 24] = 7;
    packet[8 + 25] = 1;
    using Kind = platform::WirelessEventKind;
    platform::WirelessEvent event;
    if (!platform::parse_esp32_monitor_event("D 01:02:03:04:05:06 0",
                                             Kind::Deauthentication, event) ||
        event.bssid != "01:02:03:04:05:06" || event.reason != 0 || event.protected_frame ||
        !platform::parse_esp32_monitor_event("D 01:02:03:04:05:06 protected",
                                             Kind::Deauthentication, event) ||
        !event.protected_frame ||
        platform::parse_esp32_monitor_event("D 01:02:03:04:05:06 65536",
                                            Kind::Deauthentication, event) ||
        platform::parse_esp32_monitor_event("D 01:02:03:04:05:06 7 extra",
                                            Kind::Deauthentication, event) ||
        platform::parse_esp32_monitor_event("D 01:02:03:04:05:GG 7",
                                            Kind::Deauthentication, event) ||
        !platform::parse_esp32_monitor_event("P", Kind::ProbeRequest, event) ||
        !event.bssid.empty() ||
        platform::parse_esp32_monitor_event("P client", Kind::ProbeRequest, event)) {
        std::cerr << "ESP32 event record parsing failed\n";
        return 1;
    }
    if (!platform::parse_esp32_monitor_event("P 6", Kind::ProbeRequest, event, true) ||
        event.channel != 6 ||
        !platform::parse_esp32_monitor_event("D 01:02:03:04:05:06 7 11", Kind::Deauthentication, event, true) ||
        event.channel != 11 || event.reason != 7 ||
        platform::parse_esp32_monitor_event("P 2", Kind::ProbeRequest, event, true) ||
        platform::parse_esp32_monitor_event("P", Kind::ProbeRequest, event, true) ||
        platform::parse_esp32_monitor_event("P 6", Kind::ProbeRequest, event)) {
        std::cerr << "ESP32 hopping records must include a valid channel\n";
        return 1;
    }
    if (!platform::parse_wireless_event(packet.data(), packet.size(), Kind::Deauthentication, event) ||
        event.bssid != "01:02:03:04:05:06" || event.reason != 263 || event.protected_frame ||
        platform::parse_wireless_event(packet.data(), 33, Kind::Deauthentication, event) ||
        platform::parse_wireless_event(nullptr, packet.size(), Kind::Deauthentication, event)) {
        std::cerr << "Deauthentication header parsing failed\n";
        return 1;
    }
    packet[9] = 0x40;
    if (!platform::parse_wireless_event(packet.data(), 32, Kind::Deauthentication, event) ||
        !event.protected_frame || event.reason != 0) {
        std::cerr << "Protected frame must not expose an encrypted reason\n";
        return 1;
    }
    packet[8] = 0x80;
    if (platform::parse_wireless_event(packet.data(), packet.size(), Kind::Deauthentication, event)) {
        std::cerr << "Non-deauthentication frame accepted\n";
        return 1;
    }
    std::string error;
    if (!platform::valid_monitor_interface_name("mon0") ||
        !platform::valid_monitor_interface_name("wlan0.mon-1") ||
        platform::valid_monitor_interface_name("") ||
        platform::valid_monitor_interface_name("..") ||
        platform::valid_monitor_interface_name("../wlan0") ||
        platform::valid_monitor_interface_name("0123456789abcdef")) {
        std::cerr << "Monitor interface name validation failed\n";
        return 1;
    }
    packet[8] = 0x40;
    if (!platform::parse_wireless_event(packet.data(), 32, Kind::ProbeRequest, event) ||
        !event.bssid.empty() || event.reason != 0 ||
        platform::parse_wireless_event(packet.data(), 31, Kind::ProbeRequest, event)) {
        std::cerr << "Probe request should expose no client identifiers\n";
        return 1;
    }
    if (platform::WirelessEventMonitor::start("../wlan0", {}, Kind::Deauthentication, error) ||
        error.empty()) {
        std::cerr << "Invalid interface name accepted\n";
        return 1;
    }
#if USE_DESKTOP
    if (platform::WirelessEventMonitor::start("wlan0", {}, Kind::Deauthentication, error) ||
        error.empty()) {
        std::cerr << "Desktop monitor must report unsupported\n";
        return 1;
    }
#endif
#if defined(POSEIDON_TEST_WIRELESS_EVENT) && !USE_DESKTOP
    int pair[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, pair) != 0 ||
        !platform::attach_wireless_event_test_filter(pair[1], true, Kind::Deauthentication)) {
        std::cerr << "Could not set up local filter fixture\n";
        return 1;
    }
    const auto received = [&](const std::array<uint8_t, 320>& input) {
        std::array<uint8_t, 320> output{};
        if (send(pair[0], input.data(), input.size(), 0) != static_cast<ssize_t>(input.size()))
            return ssize_t{-2};
        const auto count = recv(pair[1], output.data(), output.size(), 0);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return ssize_t{0};
        return count;
    };
    packet[8] = 0xc0;
    if (received(packet) != 0 ||
        !platform::attach_wireless_event_test_filter(pair[1], false, Kind::Deauthentication)) {
        std::cerr << "Non-radiotap socket metadata passed the filter\n";
        return 1;
    }
    packet[8] = 0x80;
    if (received(packet) != 0) {
        std::cerr << "Non-deauthentication traffic passed the filter\n";
        return 1;
    }
    packet[8] = 0xc0;
    packet[9] = 0;
    if (received(packet) != 34) {
        std::cerr << "Unprotected deauthentication was not header-limited\n";
        return 1;
    }
    packet[9] = 0x40;
    if (received(packet) != 32) {
        std::cerr << "Protected frame was not header-limited\n";
        return 1;
    }
    packet[0] = 1;
    if (received(packet) != 0) {
        std::cerr << "Unsupported radiotap version passed the filter\n";
        return 1;
    }
    packet.fill(0);
    packet[2] = 8;
    packet[3] = 1;
    packet[264] = 0xc0;
    if (received(packet) != 290) {
        std::cerr << "Long radiotap header was not handled correctly\n";
        return 1;
    }
    if (!platform::attach_wireless_event_test_filter(pair[1], false, Kind::ProbeRequest)) {
        std::cerr << "Could not attach probe filter\n";
        return 1;
    }
    packet.fill(0);
    packet[2] = 8;
    packet[8] = 0x40;
    if (received(packet) != 32) {
        std::cerr << "Probe request body was not removed\n";
        return 1;
    }
    packet[8] = 0xc0;
    if (received(packet) != 0) {
        std::cerr << "Non-probe frame passed probe filter\n";
        return 1;
    }
    close(pair[0]);
    close(pair[1]);
    auto invalid_mode = platform::WirelessEventMonitor::start(
        "lo", std::filesystem::temp_directory_path(), Kind::Deauthentication, error);
    if (!invalid_mode) {
        std::cerr << "Could not start interface validation test: " << error << '\n';
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!invalid_mode->status().complete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto status = invalid_mode->status();
    if (!status.complete || status.error.find("not radiotap") == std::string::npos ||
        status.listening || !status.file_name.empty()) {
        std::cerr << "Non-monitor interface was not rejected\n";
        return 1;
    }
#endif
    return 0;
}
