#include "storage_status.h"

#include <filesystem>

namespace platform {

StorageStatus read_storage_status() {
#if USE_DESKTOP
    return {false, false, -1, -1};
#else
    StorageStatus status;
    std::error_code error;
    const std::filesystem::path mount("/mnt");
    status.mounted = std::filesystem::is_directory(mount, error);

    std::filesystem::path sd_root("/media");
    if (std::filesystem::exists(sd_root, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(sd_root, error)) {
            if (std::filesystem::is_directory(entry.status(error))) {
                status.micro_sd_present = true;
                const auto space = std::filesystem::space(entry.path(), error);
                status.total_bytes = static_cast<int64_t>(space.capacity);
                status.free_bytes = static_cast<int64_t>(space.available);
                break;
            }
        }
    }
    return status;
#endif
}

} // namespace platform
