#include "tool_status.h"

#include <filesystem>

namespace platform {

std::vector<FileEntry> list_directory(const std::string& path) {
    std::vector<FileEntry> entries;
    std::error_code error;
    const std::filesystem::path root(path);
    if (!std::filesystem::exists(root, error) ||
        !std::filesystem::is_directory(root, error)) {
        return entries;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) {
            break;
        }
        FileEntry item;
        item.name = entry.path().filename().string();
        item.directory = entry.is_directory(error);
        if (!item.directory) {
            item.size = static_cast<int64_t>(entry.file_size(error));
        }
        entries.push_back(std::move(item));
        if (entries.size() >= 64) {
            break;
        }
    }
    return entries;
}

bool path_exists(const std::string& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

} // namespace platform
