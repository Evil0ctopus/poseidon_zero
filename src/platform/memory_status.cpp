#include "memory_status.h"

#include <cstdint>
#include <fstream>
#include <string>

namespace platform {
namespace {

int64_t read_kib_value(const char* line_prefix) {
    std::ifstream file("/proc/meminfo");
    std::string key;
    int64_t value{-1};
    std::string unit;
    while (file >> key >> value >> unit) {
        if (key == line_prefix) {
            return value;
        }
        file.ignore(4096, '\n');
    }
    return -1;
}

} // namespace

MemoryStatus read_memory_status() {
#if USE_DESKTOP
    return {16384 * 1024, 4096 * 1024, 0};
#else
    MemoryStatus status;
    status.mem_total_kib = read_kib_value("MemTotal:");
    status.mem_available_kib = read_kib_value("MemAvailable:");
    status.swap_total_kib = read_kib_value("SwapTotal:");
    return status;
#endif
}

} // namespace platform
