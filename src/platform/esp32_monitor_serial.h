#pragma once

#include "wireless_event_monitor.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace platform {

bool parse_esp32_monitor_event(const std::string& line, WirelessEventKind kind,
                               WirelessEvent& event, bool hopping = false);

class Esp32MonitorSerial {
public:
    static std::unique_ptr<Esp32MonitorSerial> open(const std::string& path,
                                                     std::atomic_bool& cancelled,
                                                     WirelessEventKind kind,
                                                     std::string& error,
                                                     unsigned channel = 1);
    ~Esp32MonitorSerial();
    bool read_event(WirelessEvent& event, bool& received, std::string& error);
    bool stop(std::string& error);

    Esp32MonitorSerial(const Esp32MonitorSerial&) = delete;
    Esp32MonitorSerial& operator=(const Esp32MonitorSerial&) = delete;

private:
    explicit Esp32MonitorSerial(std::atomic_bool& cancelled, WirelessEventKind kind);
    bool send(const char* command, std::string& error);
    bool read_line(std::string& line, int timeout_ms, std::string& error);
    bool wait_for(const char* expected, int timeout_ms, bool retry_hello, std::string& error);
    int fd_{-1};
    std::atomic_bool& cancelled_;
    WirelessEventKind kind_;
    unsigned channel_{1};
    bool started_{false};
    bool exclusive_{false};
    std::string buffer_;
    std::chrono::steady_clock::time_point last_ping_{};
    std::chrono::steady_clock::time_point last_pong_{};
#if !USE_DESKTOP
    struct SerialSettings;
    std::unique_ptr<SerialSettings> settings_;
#endif
};

} // namespace platform
