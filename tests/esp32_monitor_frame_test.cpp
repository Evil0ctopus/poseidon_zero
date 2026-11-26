#include "frame_filter.h"

#include <array>
#include <iostream>

int main() {
    char kind = 0;
    uint8_t channel = 0;
    bool hopping = false;
    if (!parse_monitor_start("START D", kind, channel, hopping) || kind != 'D' ||
        channel != 1 || hopping ||
        !parse_monitor_start("START P 11", kind, channel, hopping) || kind != 'P' ||
        channel != 11 || hopping ||
        !parse_monitor_start("START D H", kind, channel, hopping) || channel != 1 || !hopping ||
        parse_monitor_start("START D 12", kind, channel, hopping) ||
        parse_monitor_start("START D 0", kind, channel, hopping) ||
        parse_monitor_start("START D 1x", kind, channel, hopping) ||
        parse_monitor_start("START P ", kind, channel, hopping) ||
        parse_monitor_start("START X", kind, channel, hopping)) {
        std::cerr << "ESP32 start command validation failed\n";
        return 1;
    }
    std::array<uint8_t, 64> frame{};
    for (unsigned i = 0; i < 6; ++i) {
        frame[10 + i] = 0xaa;
        frame[16 + i] = static_cast<uint8_t>(i + 1);
    }
    frame[0] = 0xc0;
    frame[24] = 7;
    frame[25] = 1;
    MonitorEvent event;
    if (!decode_monitor_frame(frame.data(), frame.size(), 'D', event) ||
        event.bssid[0] != 1 || event.bssid[5] != 6 || event.reason != 263 ||
        event.protected_frame || decode_monitor_frame(frame.data(), 25, 'D', event)) {
        std::cerr << "Deauthentication frame filtering failed\n";
        return 1;
    }
    frame[1] = 0x40;
    if (!decode_monitor_frame(frame.data(), 24, 'D', event) ||
        !event.protected_frame || event.reason != 0) {
        std::cerr << "Protected reason must not be exposed\n";
        return 1;
    }
    frame[0] = 0x40;
    if (!decode_monitor_frame(frame.data(), frame.size(), 'P', event) ||
        event.bssid[0] != 0 || event.bssid[5] != 0 || event.reason != 0 ||
        decode_monitor_frame(frame.data(), 23, 'P', event) ||
        decode_monitor_frame(frame.data(), frame.size(), 'D', event) ||
        decode_monitor_frame(nullptr, frame.size(), 'P', event)) {
        std::cerr << "Probe frame filtering failed\n";
        return 1;
    }
    return 0;
}
