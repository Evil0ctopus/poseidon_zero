#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

enum class PortProtocol { Tcp, Udp };
enum class PortState { Open, Closed, Filtered, OpenOrFiltered, Unreachable, Error };

struct PortList {
    std::vector<uint16_t> ports;
    std::string error;
};

struct PortTargets {
    std::vector<std::string> addresses;
    std::string error;
};

struct PortResult {
    std::string address;
    uint16_t port{0};
    PortProtocol protocol{PortProtocol::Tcp};
    PortState state{PortState::Error};
};

struct PortScanProgress {
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> total{0};
    std::atomic_bool cancelled{false};
};

struct PortScanResult {
    std::vector<PortResult> entries;
    std::vector<std::string> targets;
    std::vector<uint16_t> ports;
    uint64_t completed{0};
    uint64_t total{0};
    bool tcp{false};
    bool udp{false};
    uint64_t open{0};
    uint64_t closed{0};
    uint64_t filtered{0};
    uint64_t open_or_filtered{0};
    uint64_t unreachable{0};
    uint64_t errors{0};
    bool truncated{false};
    bool cancelled{false};
    std::string error;
};

struct PortReportResult {
    std::filesystem::path path;
    std::string error;
};

PortList parse_port_list(const std::string& input);
PortList common_ports();
std::string format_port_list(const std::vector<uint16_t>& ports);
PortTargets resolve_port_targets(const std::string& input, const std::atomic_bool& cancelled);
PortTargets numeric_port_targets(const std::string& input);
PortScanResult scan_ports(const PortTargets& targets, const std::vector<uint16_t>& ports,
                          bool tcp, bool udp, PortScanProgress& progress);
PortReportResult save_port_report(const std::filesystem::path& directory,
                                 const PortScanResult& result);
const char* port_state_name(PortState state);

} // namespace platform
