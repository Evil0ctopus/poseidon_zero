#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

enum class WirelessEventKind { Deauthentication, ProbeRequest };
bool valid_monitor_interface_name(const std::string& interface_name);

struct WirelessEvent {
    std::string bssid;
    bool protected_frame{false};
    uint16_t reason{0};
    uint8_t channel{0};
};

bool parse_wireless_event(const uint8_t* data, std::size_t length, WirelessEventKind kind,
                          WirelessEvent& event);
#if defined(POSEIDON_TEST_WIRELESS_EVENT) && !USE_DESKTOP
bool attach_wireless_event_test_filter(int fd, bool check_packet_metadata, WirelessEventKind kind);
#endif

struct WirelessEventMonitorStatus {
    bool listening{false};
    bool complete{false};
    unsigned events{0};
    std::string last_bssid;
    std::string file_name;
    std::string error;
};

class WirelessEventMonitor {
public:
    static std::unique_ptr<WirelessEventMonitor> start(std::string interface_name,
                                                        std::filesystem::path directory,
                                                        WirelessEventKind kind,
                                                        std::string& error,
                                                        unsigned esp32_channel = 1);
    ~WirelessEventMonitor();
    void stop();
    WirelessEventMonitorStatus status() const;

    WirelessEventMonitor(const WirelessEventMonitor&) = delete;
    WirelessEventMonitor& operator=(const WirelessEventMonitor&) = delete;

private:
    WirelessEventMonitor() = default;
    void run(std::string interface_name, std::filesystem::path directory, WirelessEventKind kind,
             unsigned esp32_channel);
    std::atomic_bool cancelled_{false};
    mutable std::mutex mutex_;
    WirelessEventMonitorStatus status_;
    std::thread thread_;
};

} // namespace platform
