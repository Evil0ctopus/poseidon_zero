#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct FileEntry {
    std::string name;
    bool directory{false};
    int64_t size{-1};
};

std::vector<FileEntry> list_directory(const std::string& path);
bool path_exists(const std::string& path);

} // namespace platform
