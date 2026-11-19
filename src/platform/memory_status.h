#pragma once

#include <cstdint>

namespace platform {

struct MemoryStatus {
    int64_t mem_total_kib{-1};
    int64_t mem_available_kib{-1};
    int64_t swap_total_kib{-1};
};

MemoryStatus read_memory_status();

} // namespace platform
