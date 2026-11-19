#pragma once

#include <cstdint>

namespace platform {

struct StorageStatus {
    bool micro_sd_present{false};
    bool mounted{false};
    int64_t total_bytes{-1};
    int64_t free_bytes{-1};
};

StorageStatus read_storage_status();

} // namespace platform
