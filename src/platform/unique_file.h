#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace platform {

struct UniqueFile {
    std::filesystem::path path;
    std::unique_ptr<FILE, int (*)(FILE*)> stream{nullptr, &std::fclose};
    std::string error;
};

std::string ensure_private_directory(const std::filesystem::path& directory);
std::string ensure_confidential_directory(const std::filesystem::path& directory);
bool is_unique_filename(const std::string& name, const std::string& prefix,
                        const std::string& suffix);
UniqueFile open_unique_file(const std::filesystem::path& directory,
                            const std::string& prefix, const std::string& suffix);
std::string remove_incomplete_file(const std::filesystem::path& path);

} // namespace platform
