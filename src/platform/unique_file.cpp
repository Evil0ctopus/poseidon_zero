#include "unique_file.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace platform {

bool is_unique_filename(const std::string& name, const std::string& prefix,
                        const std::string& suffix) {
    if (name.size() <= prefix.size() + suffix.size() ||
        name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const auto middle = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    const std::size_t timestamp_start = middle[0] == '-' ? 1 : 0;
    const auto dash = middle.find('-', timestamp_start);
    if (dash == std::string::npos || dash == timestamp_start || dash + 1 == middle.size())
        return false;
    const auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    return std::all_of(middle.begin() + timestamp_start, middle.begin() + dash, digit) &&
           std::all_of(middle.begin() + dash + 1, middle.end(), digit);
}

std::string ensure_private_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
    std::error_code error;
    std::filesystem::create_directory(directory, error);
    if (error) return error.message();
    if (!std::filesystem::is_directory(directory, error))
        return error ? error.message() : "Path is not a directory";
#else
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        return std::strerror(errno);
    struct stat status{};
    if (lstat(directory.c_str(), &status) != 0) return std::strerror(errno);
    if (!S_ISDIR(status.st_mode)) return "Path is not a directory";
    if (status.st_uid != geteuid()) return "Directory must be owned by the current user";
    if ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return "Directory must not be writable by other users";
#endif
    return {};
}

std::string ensure_confidential_directory(const std::filesystem::path& directory) {
    const auto error = ensure_private_directory(directory);
    if (!error.empty()) return error;
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return "Cannot inspect directory (error " + std::to_string(GetLastError()) + ")";
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return "Directory must not be a reparse point";
#else
    struct stat status{};
    if (lstat(directory.c_str(), &status) != 0) return std::strerror(errno);
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return "Directory must not be accessible to other users";
#endif
    return {};
}

UniqueFile open_unique_file(const std::filesystem::path& directory,
                            const std::string& prefix, const std::string& suffix) {
    UniqueFile result;
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        result.error = error ? "Cannot inspect directory: " + error.message()
                             : "Directory does not exist";
        return result;
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int fd = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        result.path = directory / (prefix + std::to_string(timestamp) + "-" +
                                   std::to_string(attempt) + suffix);
#if defined(_WIN32)
        const int open_error = _wsopen_s(&fd, result.path.c_str(),
                                         _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
                                         _SH_DENYRW, _S_IREAD | _S_IWRITE);
        if (open_error != 0) {
            fd = -1;
            errno = open_error;
        }
#else
        fd = open(result.path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
#endif
        if (fd >= 0) break;
        if (errno != EEXIST) {
            result.error = std::string("Cannot create file: ") + std::strerror(errno);
            return result;
        }
    }
    if (fd < 0) {
        result.error = "Cannot create a unique filename";
        return result;
    }
#if defined(_WIN32)
    result.stream.reset(_fdopen(fd, "wb"));
#else
    result.stream.reset(fdopen(fd, "wb"));
#endif
    if (!result.stream) {
        result.error = std::string("Cannot open file: ") + std::strerror(errno);
#if defined(_WIN32)
        _close(fd);
#else
        close(fd);
#endif
        result.error += remove_incomplete_file(result.path);
    }
    return result;
}

std::string remove_incomplete_file(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    return error ? "; cleanup failed: " + error.message() : "";
}

} // namespace platform
