#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

struct MonitorEvent {
    uint8_t bssid[6]{};
    uint16_t reason{0};
    uint8_t channel{0};
    bool protected_frame{false};
};

inline bool parse_monitor_start(const char* command, char& kind, uint8_t& channel, bool& hopping) {
    if (!command || std::strncmp(command, "START ", 6) != 0 ||
        (command[6] != 'D' && command[6] != 'P')) return false;
    const char selected = command[6];
    if (command[7] == '\0') {
        kind = selected;
        channel = 1;
        hopping = false;
        return true;
    }
    if (command[7] != ' ') return false;
    const char* value = command + 8;
    if (std::strcmp(value, "H") == 0) {
        kind = selected;
        channel = 1;
        hopping = true;
        return true;
    }
    unsigned number = 0;
    if (!*value) return false;
    while (*value >= '0' && *value <= '9' && number <= 11)
        number = number * 10 + static_cast<unsigned>(*value++ - '0');
    if (*value || number < 1 || number > 11) return false;
    kind = selected;
    channel = static_cast<uint8_t>(number);
    hopping = false;
    return true;
}

inline bool decode_monitor_frame(const uint8_t* frame, std::size_t length, char kind,
                                 MonitorEvent& event) {
    if (!frame || length < 24) return false;
    MonitorEvent parsed{};
    if (kind == 'P' && frame[0] == 0x40) {
        event = parsed;
        return true;
    }
    if (kind != 'D' || frame[0] != 0xc0) return false;
    parsed.protected_frame = (frame[1] & 0x40) != 0;
    if (!parsed.protected_frame && length < 26) return false;
    std::memcpy(parsed.bssid, frame + 16, sizeof(parsed.bssid));
    if (!parsed.protected_frame) parsed.reason = frame[24] | (frame[25] << 8);
    event = parsed;
    return true;
}
