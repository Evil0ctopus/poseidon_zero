#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct NmapService {
    uint16_t port{0};
    std::string service;
    std::string version;
};

struct NmapResult {
    std::string address;
    std::vector<NmapService> services;
    std::string os_guess;
    std::string error;
    std::string warning;
    bool cancelled{false};
};

NmapResult parse_nmap_grepable(const std::string& output, const std::string& address,
                               const std::vector<uint16_t>& ports);
NmapResult run_nmap_inspection(const std::string& address, const std::vector<uint16_t>& ports,
                               bool os_detection, const std::atomic_bool& cancelled);
#if defined(POSEIDON_NMAP_TESTING)
NmapResult run_nmap_test_executable(const std::string& executable, const std::string& address,
                                    const std::vector<uint16_t>& ports, bool os_detection,
                                    const std::atomic_bool& cancelled);
#endif

} // namespace platform
