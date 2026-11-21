#include "tool_status.h"

#include <algorithm>
#include <filesystem>
#include <limits>

namespace platform {

DirectoryResult read_directory(const std::string& path,
                               std::size_t offset,
                               const std::atomic_bool* cancelled) {
    DirectoryResult result;
    if (cancelled && cancelled->load()) {
        result.error = "Directory read canceled";
        return result;
    }
    std::error_code error;
    const auto root = std::filesystem::u8path(path);
    std::filesystem::directory_iterator iterator(root, error);
    if (error) {
        result.error = "Cannot open directory: " + error.message();
        return result;
    }

    const std::filesystem::directory_iterator end;
    for (std::size_t skipped = 0; iterator != end && skipped < offset; ++skipped) {
        if (cancelled && cancelled->load()) {
            result.error = "Directory read canceled";
            return result;
        }
        iterator.increment(error);
        if (error) {
            result.error = "Cannot read directory: " + error.message();
            return result;
        }
    }
    while (iterator != end) {
        if (cancelled && cancelled->load()) {
            result.entries.clear();
            result.error = "Directory read canceled";
            return result;
        }
        if (result.entries.size() == kDirectoryPageSize) {
            result.truncated = true;
            break;
        }
        const auto& entry = *iterator;
        FileEntry item;
        item.name = entry.path().filename().u8string();
        item.directory = entry.is_directory(error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            result.entries.push_back(std::move(item));
            iterator.increment(error);
            if (error) {
                result.error = "Cannot read directory: " + error.message();
                break;
            }
            continue;
        }
        if (error) {
            result.error = "Cannot inspect entry: " + error.message();
            break;
        }
        if (!item.directory && entry.is_regular_file(error)) {
            const auto size = entry.file_size(error);
            if (!error && size <= static_cast<std::uintmax_t>(std::numeric_limits<int64_t>::max())) {
                item.size = static_cast<int64_t>(size);
            }
        }
        if (error) {
            result.error = "Cannot inspect entry: " + error.message();
            break;
        }
        result.entries.push_back(std::move(item));
        iterator.increment(error);
        if (error) {
            result.error = "Cannot read directory: " + error.message();
            break;
        }
    }
    if (!result.error.empty()) {
        result.entries.clear();
        result.truncated = false;
        return result;
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const FileEntry& left, const FileEntry& right) {
                  return left.directory != right.directory ? left.directory > right.directory
                                                            : left.name < right.name;
              });
    return result;
}

std::vector<FileEntry> list_directory(const std::string& path) {
    return read_directory(path).entries;
}

bool path_exists(const std::string& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

} // namespace platform
