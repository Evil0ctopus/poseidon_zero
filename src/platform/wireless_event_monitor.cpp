#include "wireless_event_monitor.h"

#include "esp32_monitor_serial.h"
#include "unique_file.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <system_error>

#if !USE_DESKTOP
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace platform {

#if !USE_DESKTOP
namespace {

bool attach_filter(int fd, bool check_packet_metadata, WirelessEventKind kind) {
    sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 static_cast<uint32_t>(SKF_AD_OFF + SKF_AD_HATYPE)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARPHRD_IEEE80211_RADIOTAP, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 static_cast<uint32_t>(SKF_AD_OFF + SKF_AD_PKTTYPE)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PACKET_OUTGOING, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 1),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 2),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 3),
        BPF_STMT(BPF_ALU | BPF_LSH | BPF_K, 8),
        BPF_STMT(BPF_LDX | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_ADD | BPF_X, 0),
        BPF_STMT(BPF_MISC | BPF_TAX, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 8, 0, 10),
        BPF_STMT(BPF_LD | BPF_B | BPF_IND, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                 kind == WirelessEventKind::Deauthentication ? 0xc0U : 0x40U, 0, 8),
        BPF_STMT(BPF_LD | BPF_B | BPF_IND, 1),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40, 3, 0),
        BPF_STMT(BPF_MISC | BPF_TXA, 0),
        BPF_STMT(BPF_ALU | BPF_ADD | BPF_K,
                 kind == WirelessEventKind::Deauthentication ? 26U : 24U),
        BPF_STMT(BPF_RET | BPF_A, 0),
        BPF_STMT(BPF_MISC | BPF_TXA, 0),
        BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 24),
        BPF_STMT(BPF_RET | BPF_A, 0),
        BPF_STMT(BPF_RET | BPF_K, 0),
    };
    const unsigned offset = check_packet_metadata ? 0 : 6;
    sock_fprog filter{
        static_cast<unsigned short>(sizeof(instructions) / sizeof(instructions[0]) - offset),
        instructions + offset};
    return setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &filter, sizeof(filter)) == 0;
}

} // namespace

#if defined(POSEIDON_TEST_WIRELESS_EVENT)
bool attach_wireless_event_test_filter(int fd, bool check_packet_metadata, WirelessEventKind kind) {
    return attach_filter(fd, check_packet_metadata, kind);
}
#endif
#endif

bool valid_monitor_interface_name(const std::string& interface_name) {
    if (interface_name.empty() || interface_name.size() > 15 ||
        interface_name == "." || interface_name == "..") return false;
    for (const char c : interface_name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

bool parse_wireless_event(const uint8_t* data, std::size_t length, WirelessEventKind kind,
                          WirelessEvent& event) {
    if (!data || length < 8 || data[0] != 0 || data[1] != 0) return false;
    const std::size_t radiotap_length = data[2] | (static_cast<std::size_t>(data[3]) << 8);
    if (radiotap_length < 8 || radiotap_length > length ||
        length - radiotap_length < 24) return false;
    const auto* frame = data + radiotap_length;
    if (frame[0] != (kind == WirelessEventKind::Deauthentication ? 0xc0 : 0x40)) return false;
    if (kind == WirelessEventKind::ProbeRequest) {
        event = {};
        return true;
    }
    const bool protected_frame = (frame[1] & 0x40) != 0;
    if (!protected_frame && length - radiotap_length < 26) return false;
    char bssid[18]{};
    std::snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                  frame[16], frame[17], frame[18], frame[19], frame[20], frame[21]);
    event.bssid = bssid;
    event.channel = 0;
    event.protected_frame = protected_frame;
    event.reason = event.protected_frame ? 0 :
        static_cast<uint16_t>(frame[24] | (static_cast<uint16_t>(frame[25]) << 8));
    return true;
}

std::unique_ptr<WirelessEventMonitor> WirelessEventMonitor::start(
    std::string interface_name, std::filesystem::path directory,
    WirelessEventKind kind, std::string& error, unsigned esp32_channel) {
    error.clear();
    if (!valid_monitor_interface_name(interface_name)) {
        error = "Invalid monitor interface name";
        return nullptr;
    }
    if (esp32_channel > 11 || (interface_name != "esp32" && esp32_channel != 1)) {
        error = "Invalid ESP32 channel selection";
        return nullptr;
    }
#if USE_DESKTOP
    (void)directory;
    (void)kind;
    (void)esp32_channel;
    error = "Wireless monitor unavailable in desktop simulator";
    return nullptr;
#else
    auto session = std::unique_ptr<WirelessEventMonitor>(new WirelessEventMonitor);
    try {
        session->thread_ = std::thread(&WirelessEventMonitor::run, session.get(),
                                       std::move(interface_name), std::move(directory), kind, esp32_channel);
    } catch (const std::system_error& failure) {
        error = "Cannot start monitor: " + failure.code().message();
        return nullptr;
    }
    return session;
#endif
}

WirelessEventMonitor::~WirelessEventMonitor() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void WirelessEventMonitor::stop() { cancelled_ = true; }

WirelessEventMonitorStatus WirelessEventMonitor::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void WirelessEventMonitor::run(std::string interface_name, std::filesystem::path directory,
                               WirelessEventKind kind, unsigned esp32_channel) {
#if !USE_DESKTOP
    std::string error;
    int fd = -1;
    std::unique_ptr<Esp32MonitorSerial> esp32;
    try {
        unsigned index = 0;
        if (interface_name == "esp32") {
            const char* path = std::getenv("POSEIDON_ESP32_MONITOR_DEVICE");
            if (!path || !*path) error = "Set POSEIDON_ESP32_MONITOR_DEVICE";
            else if (!cancelled_) esp32 = Esp32MonitorSerial::open(path, cancelled_, kind, error, esp32_channel);
        } else {
            const auto type_path = std::filesystem::path("/sys/class/net") / interface_name / "type";
            std::ifstream type_file(type_path);
            unsigned type = 0;
            if (!(type_file >> type))
                error = "Cannot read interface type";
            else if (type != ARPHRD_IEEE80211_RADIOTAP)
                error = "Interface is not radiotap monitor mode";
            index = error.empty() ? if_nametoindex(interface_name.c_str()) : 0;
            if (error.empty() && index == 0) error = "Monitor interface not found";
            if (error.empty() && !cancelled_) {
                fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
                if (fd < 0) error = std::string("Packet socket: ") + std::strerror(errno);
            }
            if (error.empty() && !cancelled_ && !attach_filter(fd, true, kind))
                error = std::string("Attach packet filter: ") + std::strerror(errno);
        }
        UniqueFile log;
        bool log_created = false;
        bool header_written = false;
        if (error.empty() && !cancelled_) {
            error = ensure_private_directory(directory);
            if (error.empty()) {
                log = open_unique_file(directory, kind == WirelessEventKind::Deauthentication
                    ? "deauth-monitor-" : "probe-monitor-", ".txt");
                error = log.error;
                log_created = static_cast<bool>(log.stream);
            }
        }
        if (error.empty() && !cancelled_) {
            const char* description = kind == WirelessEventKind::Deauthentication
                ? "deauthentication\nUnix UTC seconds, unverified BSSID, reason (or protected).\n"
                : esp32 && esp32_channel == 0
                ? "probe requests\nUnix UTC seconds and channel; no client addresses or names.\n"
                : "probe requests\nUnix UTC seconds only; no client addresses or names.\n";
            const std::string channel_description = !esp32 ? "" : esp32_channel == 0
                ? " (2.4 GHz hopping 1,6,11; rows include channel)"
                : " (2.4 GHz channel " + std::to_string(esp32_channel) + ")";
            if (std::fprintf(log.stream.get(),
                             "POSEIDON passive wireless monitor: %s"
                             "Interface: %s%s\nIncomplete observations; no client addresses "
                             "or payload saved.\n", description, interface_name.c_str(),
                             channel_description.c_str()) < 0 ||
                std::fflush(log.stream.get()) != 0)
                error = std::string("Write log: ") + std::strerror(errno);
            else
                header_written = true;
        }
        if (header_written) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.file_name = log.path.filename().string();
        }
        sockaddr_ll binding{};
        binding.sll_family = AF_PACKET;
        binding.sll_protocol = htons(ETH_P_ALL);
        binding.sll_ifindex = static_cast<int>(index);
        if (!esp32 && error.empty() && !cancelled_ &&
            bind(fd, reinterpret_cast<sockaddr*>(&binding), sizeof(binding)) != 0)
            error = std::string("Bind monitor: ") + std::strerror(errno);
        if (error.empty() && !cancelled_) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.listening = true;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        unsigned count = 0;
        while (error.empty() && !cancelled_ && count < 1000 &&
               std::chrono::steady_clock::now() < deadline) {
            WirelessEvent event;
            if (esp32) {
                bool received = false;
                if (!esp32->read_event(event, received, error)) break;
                if (!received) continue;
            } else {
                pollfd ready{fd, POLLIN, 0};
                const int polled = poll(&ready, 1, 200);
                if (polled < 0) {
                    if (errno == EINTR) continue;
                    error = std::string("Poll: ") + std::strerror(errno);
                    break;
                }
                if (polled == 0) continue;
                if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    error = "Monitor socket failed";
                    break;
                }
                std::array<uint8_t, 2048> packet{};
                sockaddr_ll peer{};
                socklen_t peer_length = sizeof(peer);
                const auto received = recvfrom(fd, packet.data(), packet.size(), 0,
                                               reinterpret_cast<sockaddr*>(&peer), &peer_length);
                if (received < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                    error = std::string("Receive: ") + std::strerror(errno);
                    break;
                }
                if (peer.sll_hatype != ARPHRD_IEEE80211_RADIOTAP ||
                    peer.sll_pkttype == PACKET_OUTGOING ||
                    !parse_wireless_event(packet.data(), static_cast<std::size_t>(received),
                                          kind, event))
                    continue;
            }
            const auto time = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::string detail = kind == WirelessEventKind::ProbeRequest ? "" :
                " " + event.bssid + " " +
                (event.protected_frame ? "protected" : std::to_string(event.reason));
            if (esp32 && esp32_channel == 0) detail += " ch=" + std::to_string(event.channel);
            if (std::fprintf(log.stream.get(), "%lld%s\n",
                             static_cast<long long>(time), detail.c_str()) < 0 ||
                std::fflush(log.stream.get()) != 0) {
                error = std::string("Write log: ") + std::strerror(errno);
                break;
            }
            ++count;
            std::lock_guard<std::mutex> lock(mutex_);
            status_.events = count;
            status_.last_bssid = event.bssid;
        }
        if (esp32) {
            std::string stop_error;
            if (!esp32->stop(stop_error)) {
                if (!error.empty()) error += "; ";
                error += stop_error;
            }
        }
        if (header_written && error.empty()) {
            const char* reason = cancelled_ ? "cancelled" :
                                 count >= 1000 ? "event limit" : "time limit";
            if (std::fprintf(log.stream.get(), "Stopped: %s\n", reason) < 0 ||
                std::fflush(log.stream.get()) != 0)
                error = std::string("Write log: ") + std::strerror(errno);
        } else if (header_written && esp32) {
            if (std::fprintf(log.stream.get(), "Stopped: receiver error; see application.\n") < 0 ||
                std::fflush(log.stream.get()) != 0)
                error += "; Write log: " + std::string(std::strerror(errno));
        }
        if (log.stream && std::fclose(log.stream.release()) != 0 && error.empty())
            error = std::string("Close log: ") + std::strerror(errno);
        if (log_created && !header_written) error += remove_incomplete_file(log.path);
    } catch (const std::exception& failure) {
        error = std::string("Monitor failed: ") + failure.what();
    }
    if (esp32) {
        std::string stop_error;
        if (!esp32->stop(stop_error)) {
            if (!error.empty()) error += "; ";
            error += stop_error;
        }
    }
    if (fd >= 0) close(fd);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.listening = false;
        status_.complete = true;
        status_.error = error;
    }
#else
    (void)interface_name;
    (void)directory;
    (void)kind;
    (void)esp32_channel;
#endif
}

} // namespace platform
