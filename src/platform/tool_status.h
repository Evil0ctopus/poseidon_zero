#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct FileEntry {
    std::string name;
    bool directory{false};
    int64_t size{-1};
};

struct DirectoryResult {
    std::vector<FileEntry> entries;
    std::string error;
    bool truncated{false};
};

inline constexpr std::size_t kDirectoryPageSize = 64;

DirectoryResult read_directory(const std::string& path,
                               std::size_t offset = 0,
                               const std::atomic_bool* cancelled = nullptr);
std::vector<FileEntry> list_directory(const std::string& path);
bool path_exists(const std::string& path);

} // namespace platform
