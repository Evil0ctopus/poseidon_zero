#include "esp32_monitor_serial.h"

#include <pty.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

bool exchange(platform::WirelessEventKind kind, bool report_drop, bool lose_heartbeat = false,
              unsigned channel = 1, bool old_firmware = false) {
    int master = -1;
    int slave = -1;
    char name[128]{};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) return false;
    close(slave);
    std::atomic_bool cancelled{false};
    std::atomic_bool emulator_ok{true};
    std::thread emulator([&] {
        std::string input;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd ready{master, POLLIN, 0};
            const int result = poll(&ready, 1, 50);
            if (result <= 0 || !(ready.revents & POLLIN)) continue;
            char bytes[128];
            const auto count = read(master, bytes, sizeof(bytes));
            if (count <= 0) continue;
            input.append(bytes, static_cast<std::size_t>(count));
            for (auto end = input.find('\n'); end != std::string::npos; end = input.find('\n')) {
                const auto line = input.substr(0, end);
                input.erase(0, end + 1);
                std::string reply;
                if (line == "HELLO") reply = "boot text\r\nPOSEIDON-C5 1\r\n";
                else if (line == (std::string(kind == platform::WirelessEventKind::ProbeRequest
                                      ? "START P" : "START D") +
                                      (channel == 1 ? "" : channel == 0 ? " H" : " " + std::to_string(channel)))) {
                    if (old_firmware) reply = "ERROR command\n";
                    else {
                        reply = channel == 0 ? "ACTIVE H\n" : "ACTIVE " + std::to_string(channel) + "\n";
                        reply += report_drop ? "DROP 3\n" :
                            kind == platform::WirelessEventKind::ProbeRequest
                                ? (channel == 0 ? "P 6\n" : "P\n")
                                : (channel == 0 ? "D 01:02:03:04:05:06 7 6\n" :
                                                  "D 01:02:03:04:05:06 7\n");
                    }
                } else if (line == "STOP") {
                    reply = "STOPPED\n";
                    if (write(master, reply.data(), reply.size()) != static_cast<ssize_t>(reply.size()))
                        emulator_ok = false;
                    return;
                } else if (line == "PING") {
                    if (!lose_heartbeat) reply = "PONG\n";
                } else {
                    emulator_ok = false;
                    return;
                }
                if (!reply.empty() &&
                    write(master, reply.data(), reply.size()) != static_cast<ssize_t>(reply.size())) {
                    emulator_ok = false;
                    return;
                }
            }
        }
        emulator_ok = false;
    });
    std::string error;
    auto serial = platform::Esp32MonitorSerial::open(name, cancelled, kind, error, channel);
    bool ok = old_firmware ? !serial && error.find("ESP32 ERROR command") != std::string::npos
                           : static_cast<bool>(serial);
    if (serial) {
        platform::WirelessEvent event;
        bool received = false;
        bool read_ok = true;
        for (int i = 0; i < 20 && read_ok && !received; ++i)
            read_ok = serial->read_event(event, received, error);
        if (report_drop) {
            ok = !read_ok && error.find("dropped events") != std::string::npos;
        } else {
            ok = read_ok && received && event.channel == (channel == 0 ? 6 : channel) &&
                (kind == platform::WirelessEventKind::ProbeRequest
                     ? event.bssid.empty()
                     : event.bssid == "01:02:03:04:05:06" && event.reason == 7);
            if (ok && lose_heartbeat) {
                for (int i = 0; i < 30 && read_ok; ++i)
                    read_ok = serial->read_event(event, received, error);
                ok = !read_ok && error.find("heartbeat timed out") != std::string::npos;
            }
        }
        std::string stop_error;
        if (!serial->stop(stop_error)) {
            std::cerr << "Stop failed: " << stop_error << '\n';
            ok = false;
        }
        serial.reset();
    } else if (!old_firmware) {
        std::cerr << "Open failed: " << error << '\n';
    }
    emulator.join();
    close(master);
    return ok && emulator_ok;
}

bool monitor_session(unsigned channel = 1) {
    int master = -1;
    int slave = -1;
    char name[128]{};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) return false;
    close(slave);
    char directory[] = "/tmp/poseidon-esp32-test-XXXXXX";
    if (!mkdtemp(directory)) {
        close(master);
        return false;
    }
    setenv("POSEIDON_ESP32_MONITOR_DEVICE", name, 1);
    std::atomic_bool emulator_ok{true};
    std::thread emulator([&] {
        std::string input;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd ready{master, POLLIN, 0};
            if (poll(&ready, 1, 50) <= 0 || !(ready.revents & POLLIN)) continue;
            char bytes[128];
            const auto count = read(master, bytes, sizeof(bytes));
            if (count <= 0) continue;
            input.append(bytes, static_cast<std::size_t>(count));
            for (auto end = input.find('\n'); end != std::string::npos; end = input.find('\n')) {
                const auto line = input.substr(0, end);
                input.erase(0, end + 1);
                std::string reply;
                if (line == "HELLO") reply = "POSEIDON-C5 1\n";
                else if (line == (channel == 0 ? "START D H" : "START D"))
                    reply = channel == 0 ? "ACTIVE H\nD 01:02:03:04:05:06 7 6\n" :
                                           "ACTIVE 1\nD 01:02:03:04:05:06 7\n";
                else if (line == "PING") reply = "PONG\n";
                else if (line == "STOP") reply = "STOPPED\n";
                else {
                    emulator_ok = false;
                    return;
                }
                if (write(master, reply.data(), reply.size()) != static_cast<ssize_t>(reply.size())) {
                    emulator_ok = false;
                    return;
                }
                if (line == "STOP") return;
            }
        }
        emulator_ok = false;
    });
    std::string error;
    auto monitor = platform::WirelessEventMonitor::start(
        "esp32", directory, platform::WirelessEventKind::Deauthentication, error, channel);
    bool ok = static_cast<bool>(monitor);
    platform::WirelessEventMonitorStatus status;
    if (monitor) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            status = monitor->status();
            if (status.events == 1 || status.complete) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        ok = status.listening && status.events == 1 && status.last_bssid == "01:02:03:04:05:06";
        monitor->stop();
        const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            status = monitor->status();
            if (status.complete) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < stop_deadline);
        ok = ok && status.complete && status.error.empty() && !status.file_name.empty();
        monitor.reset();
    }
    emulator.join();
    close(master);
    unsetenv("POSEIDON_ESP32_MONITOR_DEVICE");
    if (!status.file_name.empty()) {
        const auto log = std::filesystem::path(directory) / status.file_name;
        std::ifstream input(log);
        const std::string contents{std::istreambuf_iterator<char>(input), {}};
        ok = ok && contents.find(channel == 0 ? "hopping 1,6,11" : "2.4 GHz channel 1") != std::string::npos &&
             contents.find("01:02:03:04:05:06 7") != std::string::npos &&
             (channel != 0 || contents.find("ch=6") != std::string::npos) &&
             contents.find("Stopped: cancelled") != std::string::npos;
        std::filesystem::remove(log);
    }
    std::filesystem::remove(directory);
    return ok && emulator_ok;
}

} // namespace

int main() {
    if (!exchange(platform::WirelessEventKind::Deauthentication, false) ||
        !exchange(platform::WirelessEventKind::ProbeRequest, false) ||
        !exchange(platform::WirelessEventKind::ProbeRequest, true) ||
        !exchange(platform::WirelessEventKind::ProbeRequest, false, true) ||
        !exchange(platform::WirelessEventKind::Deauthentication, false, false, 6) ||
        !exchange(platform::WirelessEventKind::ProbeRequest, false, false, 0) ||
        !exchange(platform::WirelessEventKind::Deauthentication, false, false, 0) ||
        !exchange(platform::WirelessEventKind::Deauthentication, false, false, 6, true) ||
        !monitor_session() || !monitor_session(0)) {
        std::cerr << "ESP32 serial protocol fixture failed\n";
        return 1;
    }
    return 0;
}
