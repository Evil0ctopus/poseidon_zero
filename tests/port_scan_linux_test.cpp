#include "port_scan.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace {

int bound_socket(int type, uint16_t& port) {
    const int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close(fd);
        return -1;
    }
    port = ntohs(address.sin_port);
    return fd;
}

bool has_result(const platform::PortScanResult& result, uint16_t port,
                platform::PortProtocol protocol, platform::PortState state) {
    for (const auto& entry : result.entries) {
        if (entry.port == port && entry.protocol == protocol && entry.state == state) return true;
    }
    return false;
}

} // namespace

int main() {
    uint16_t tcp_port = 0, udp_port = 0, silent_port = 0, closed_port = 0, closed_udp_port = 0;
    const int tcp = bound_socket(SOCK_STREAM, tcp_port);
    const int udp = bound_socket(SOCK_DGRAM, udp_port);
    const int silent = bound_socket(SOCK_DGRAM, silent_port);
    const int closed = bound_socket(SOCK_STREAM, closed_port);
    const int closed_udp = bound_socket(SOCK_DGRAM, closed_udp_port);
    if (tcp < 0 || udp < 0 || silent < 0 || closed < 0 || closed_udp < 0 || listen(tcp, 2) != 0) {
        std::cerr << "Cannot create loopback scan fixtures\n";
        return 1;
    }
    close(closed);
    close(closed_udp);
    std::thread responder([udp] {
        pollfd event{udp, POLLIN, 0};
        if (poll(&event, 1, 5000) > 0) {
            sockaddr_storage peer{};
            socklen_t length = sizeof(peer);
            char byte = 0;
            if (recvfrom(udp, &byte, 1, 0, reinterpret_cast<sockaddr*>(&peer), &length) >= 0)
                sendto(udp, "x", 1, 0, reinterpret_cast<sockaddr*>(&peer), length);
        }
    });
    const std::atomic_bool running{false};
    const auto targets = platform::resolve_port_targets("127.0.0.1", running);
    platform::PortScanProgress tcp_progress;
    const auto tcp_result = platform::scan_ports(targets, {tcp_port, closed_port}, true, false,
                                                 tcp_progress);
    platform::PortScanProgress udp_progress;
    const auto start = std::chrono::steady_clock::now();
    const auto udp_result = platform::scan_ports(targets, {udp_port, silent_port, closed_udp_port}, false, true,
                                                 udp_progress);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    responder.join();
    close(tcp);
    close(udp);
    bool passed = tcp_result.error.empty() && udp_result.error.empty() &&
                  tcp_progress.completed == 2 && udp_progress.completed == 3 &&
                  elapsed >= std::chrono::milliseconds(900) &&
                  has_result(tcp_result, tcp_port, platform::PortProtocol::Tcp, platform::PortState::Open) &&
                  has_result(tcp_result, closed_port, platform::PortProtocol::Tcp, platform::PortState::Closed) &&
                  has_result(udp_result, udp_port, platform::PortProtocol::Udp, platform::PortState::Open) &&
                  has_result(udp_result, closed_udp_port, platform::PortProtocol::Udp, platform::PortState::Closed) &&
                  has_result(udp_result, silent_port, platform::PortProtocol::Udp,
                             platform::PortState::OpenOrFiltered);
    if (!passed) std::cerr << "Loopback TCP/UDP classification failed\n";
    platform::PortScanProgress cancelled;
    cancelled.cancelled = true;
    const auto cancelled_result = platform::scan_ports(targets, {tcp_port}, true, false, cancelled);
    if (!cancelled_result.cancelled || cancelled.completed != 0) {
        std::cerr << "Cancelled scan must not start probes\n";
        passed = false;
    }
    platform::PortScanProgress during_scan;
    std::thread canceller([&during_scan] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        during_scan.cancelled = true;
    });
    const auto partial = platform::scan_ports(targets, {silent_port, udp_port}, false, true,
                                              during_scan);
    canceller.join();
    if (!partial.cancelled || during_scan.completed >= during_scan.total) {
        std::cerr << "Active scan did not cancel promptly\n";
        passed = false;
    }
    close(silent);
    char name[] = "/tmp/poseidon-port-test-XXXXXX";
    const char* made = mkdtemp(name);
    if (!made) {
        std::cerr << "Cannot create report fixture\n";
        return 1;
    }
    const auto report = platform::save_port_report(std::filesystem::path(made) / "reports", udp_result);
    struct stat status{};
    if (!report.error.empty() || stat(report.path.c_str(), &status) != 0 ||
        (status.st_mode & 0777) != 0600) {
        std::cerr << "Report creation failed or was not private\n";
        passed = false;
    }
    std::ifstream input(report.path);
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (contents.find("Protocols: UDP") == std::string::npos ||
        contents.find("Completed: 3/3") == std::string::npos ||
        contents.find("127.0.0.1") == std::string::npos ||
        contents.find("open or filtered") == std::string::npos ||
        platform::save_port_report(report.path / "invalid", udp_result).error.empty()) {
        std::cerr << "Report content or error handling failed\n";
        passed = false;
    }
    std::error_code error;
    if (!report.path.empty()) std::filesystem::remove(report.path, error);
    if (error) passed = false;
    std::filesystem::remove(std::filesystem::path(made) / "reports", error);
    if (error) passed = false;
    std::filesystem::remove(made, error);
    if (error) passed = false;
    return passed ? 0 : 1;
}
