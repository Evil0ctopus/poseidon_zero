#include "evidence_export.h"
#include "unique_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif !USE_DESKTOP
#include <fcntl.h>
#include <glib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

bool valid_source_name(const std::string& name, EvidenceSource source) {
    const std::string prefix = source == EvidenceSource::Inventory ? "poseidon-host-discovery-" :
                               source == EvidenceSource::Survey ? "survey-" :
                               source == EvidenceSource::LabListener ? "lab-listener-" :
                               source == EvidenceSource::DeauthMonitor ? "deauth-monitor-" :
                               source == EvidenceSource::ProbeMonitor ? "probe-monitor-" :
                               source == EvidenceSource::Adsb ? "adsb-receiver-" : "";
    if (prefix.empty()) return false;
    const std::string suffix = source == EvidenceSource::Survey || source == EvidenceSource::Adsb
                                   ? ".csv" : ".txt";
    return is_unique_filename(name, prefix, suffix);
}

bool valid_export_name(const std::string& name) {
    const std::string copy_prefix = "evidence-copy-", manifest_prefix = "evidence-manifest-",
                      gps_prefix = "survey-gps-", settings_prefix = "settings-backup-";
    const std::string* prefix = nullptr;
    std::string suffix;
    if (name.compare(0, copy_prefix.size(), copy_prefix) == 0) {
        prefix = &copy_prefix;
        suffix = ".bin";
    } else if (name.compare(0, manifest_prefix.size(), manifest_prefix) == 0) {
        prefix = &manifest_prefix;
        suffix = ".txt";
    } else if (name.compare(0, gps_prefix.size(), gps_prefix) == 0) {
        prefix = &gps_prefix;
        suffix = ".gpx";
    } else if (name.compare(0, settings_prefix.size(), settings_prefix) == 0) {
        prefix = &settings_prefix;
        suffix = ".ini";
    } else return false;
    return is_unique_filename(name, *prefix, suffix);
}

std::string security_directory_error(const std::filesystem::path& directory, bool& missing) {
    missing = false;
    std::error_code failure;
    const auto status = std::filesystem::symlink_status(directory, failure);
    if (failure == std::errc::no_such_file_or_directory ||
        (!failure && status.type() == std::filesystem::file_type::not_found)) {
        missing = true;
        return {};
    }
    if (failure) return "Cannot inspect log directory: " + failure.message();
    if (!std::filesystem::is_directory(status)) return "Log directory is not a regular directory";
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return "Cannot inspect log directory attributes";
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return "Log directory is a reparse point";
#endif
    const auto error = ensure_private_directory(directory);
    return error.empty() ? "" : "Log directory: " + error;
}

#if defined(_WIN32)
class Hash {
public:
    ~Hash() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    bool start() {
        DWORD length = 0, received = 0;
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length),
                              sizeof(length), &received, 0) < 0 || !length) return false;
        storage_.resize(length);
        return BCryptCreateHash(algorithm_, &hash_, storage_.data(), length, nullptr, 0, 0) >= 0;
    }
    bool update(unsigned char* data, std::size_t size) {
        return BCryptHashData(hash_, data, static_cast<ULONG>(size), 0) >= 0;
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_{nullptr};
    BCRYPT_HASH_HANDLE hash_{nullptr};
    std::vector<unsigned char> storage_;
};
#elif !USE_DESKTOP
class Hash {
public:
    ~Hash() { if (hash_) g_checksum_free(hash_); }
    bool start() { hash_ = g_checksum_new(G_CHECKSUM_SHA256); return hash_ != nullptr; }
    bool update(const unsigned char* data, std::size_t size) {
        g_checksum_update(hash_, data, static_cast<gssize>(size));
        return true;
    }
    std::string finish() {
        const char* text = g_checksum_get_string(hash_);
        return text ? text : "";
    }
private:
    GChecksum* hash_{nullptr};
};
#endif

} // namespace

FileHashResult hash_local_file(const std::filesystem::path& path, const std::atomic_bool& cancelled) {
    FileHashResult result;
    if (cancelled) { result.error = "Hash cancelled"; return result; }
#if !defined(_WIN32) && USE_DESKTOP
    (void)path;
    result.error = "File hashing is unavailable on this desktop";
    return result;
#else
    constexpr std::size_t limit = 10 * 1024 * 1024;
#if defined(_WIN32)
    HANDLE input = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (input == INVALID_HANDLE_VALUE) { result.error = "Cannot open file"; return result; }
    BY_HANDLE_FILE_INFORMATION initial{};
    if (GetFileType(input) != FILE_TYPE_DISK || !GetFileInformationByHandle(input, &initial) ||
        (initial.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        initial.nFileSizeHigh || initial.nFileSizeLow > limit) {
        CloseHandle(input); result.error = "File must be regular and at most 10 MiB"; return result;
    }
#else
    const int input = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) { result.error = std::string("Cannot open file: ") + std::strerror(errno); return result; }
    struct stat initial{};
    if (fstat(input, &initial) != 0 || !S_ISREG(initial.st_mode) || initial.st_size < 0 ||
        static_cast<std::size_t>(initial.st_size) > limit) {
        close(input); result.error = "File must be regular and at most 10 MiB"; return result;
    }
#endif
    Hash hash;
    if (!hash.start()) result.error = "Cannot initialize SHA-256";
    std::array<unsigned char, 8192> buffer{};
    std::size_t total = 0;
    while (result.error.empty()) {
        if (cancelled) { result.error = "Hash cancelled"; break; }
#if defined(_WIN32)
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            result.error = "Cannot read file"; break;
        }
#else
        const auto count = read(input, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result.error = std::string("Cannot read file: ") + std::strerror(errno); break; }
#endif
        if (!count) break;
        total += static_cast<std::size_t>(count);
        if (total > limit) { result.error = "File exceeds 10 MiB"; break; }
        if (!hash.update(buffer.data(), static_cast<std::size_t>(count))) {
            result.error = "Cannot update SHA-256"; break;
        }
    }
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION final_info{};
    if (result.error.empty() &&
        (!GetFileInformationByHandle(input, &final_info) ||
         final_info.nFileSizeHigh != initial.nFileSizeHigh ||
         final_info.nFileSizeLow != initial.nFileSizeLow ||
         CompareFileTime(&final_info.ftLastWriteTime, &initial.ftLastWriteTime) != 0 ||
         total != initial.nFileSizeLow))
        result.error = "File changed while hashing";
    if (!CloseHandle(input) && result.error.empty()) result.error = "Cannot close file";
#else
    struct stat final_status{};
    if (result.error.empty() &&
        (fstat(input, &final_status) != 0 || final_status.st_size != initial.st_size ||
         final_status.st_mtim.tv_sec != initial.st_mtim.tv_sec ||
         final_status.st_mtim.tv_nsec != initial.st_mtim.tv_nsec ||
         final_status.st_ctim.tv_sec != initial.st_ctim.tv_sec ||
         final_status.st_ctim.tv_nsec != initial.st_ctim.tv_nsec ||
         total != static_cast<std::size_t>(initial.st_size)))
        result.error = "File changed while hashing";
    if (close(input) != 0 && result.error.empty())
        result.error = std::string("Cannot close file: ") + std::strerror(errno);
#endif
    if (result.error.empty()) {
        result.sha256 = hash.finish();
        if (result.sha256.empty()) result.error = "Cannot finish SHA-256";
    }
    return result;
#endif
}

std::filesystem::path security_log_directory(const std::filesystem::path& home,
                                             EvidenceSource source) {
    if (source == EvidenceSource::LabListener) return home / "poseidon-lab";
    if (source == EvidenceSource::DeauthMonitor || source == EvidenceSource::ProbeMonitor)
        return home / "poseidon-wireless-monitor";
    if (source == EvidenceSource::Adsb) return home / "poseidon-adsb";
    return {};
}

SecurityLogList list_security_logs(const std::filesystem::path& home) {
    SecurityLogList result;
    const std::array<EvidenceSource, 4> sources{EvidenceSource::LabListener,
        EvidenceSource::DeauthMonitor, EvidenceSource::ProbeMonitor, EvidenceSource::Adsb};
    for (const auto source : sources) {
        const auto directory = security_log_directory(home, source);
        bool missing = false;
        result.error = security_directory_error(directory, missing);
        if (!result.error.empty()) break;
        if (missing) continue;
        std::error_code failure;
        std::size_t visited = 0;
        const std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator it(directory, failure);
             !failure && it != end; it.increment(failure)) {
            if (++visited > 4096) {
                result.error = "Too many directory entries (limit 4096)";
                break;
            }
            const auto name = it->path().filename().string();
            if (!valid_source_name(name, source)) continue;
            const auto status = it->symlink_status(failure);
            if (failure) break;
            if (!std::filesystem::is_regular_file(status)) continue;
#if defined(_WIN32)
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                result.error = "Cannot inspect log attributes";
                break;
            }
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
#endif
            if (result.entries.size() == 256) {
                result.error = "Too many security logs (limit 256)";
                break;
            }
            result.entries.push_back({name, source});
        }
        if (failure) result.error = "Cannot list security logs: " + failure.message();
        if (!result.error.empty()) break;
    }
    if (!result.error.empty()) {
        result.entries.clear();
        return result;
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const SecurityLogEntry& a, const SecurityLogEntry& b) {
                  return a.filename > b.filename;
              });
    return result;
}

namespace {
std::string delete_managed_file(const std::filesystem::path& directory, const std::string& filename);
}

std::string delete_security_log(const std::filesystem::path& directory,
                                const std::string& filename, EvidenceSource source) {
    if ((source != EvidenceSource::LabListener && source != EvidenceSource::DeauthMonitor &&
         source != EvidenceSource::ProbeMonitor && source != EvidenceSource::Adsb) ||
        !valid_source_name(filename, source))
        return "Invalid security log name";
    return delete_managed_file(directory, filename);
}

namespace {
std::string delete_managed_file(const std::filesystem::path& directory, const std::string& filename) {
    bool missing = false;
    const auto directory_error = security_directory_error(directory, missing);
    if (!directory_error.empty()) return directory_error;
    if (missing) return "Log directory not found";
#if !defined(_WIN32) && !USE_DESKTOP
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return std::string("Cannot open log directory: ") + std::strerror(errno);
    struct stat directory_stat{};
    if (fstat(fd, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode) ||
        directory_stat.st_uid != geteuid() || (directory_stat.st_mode & (S_IWGRP | S_IWOTH))) {
        close(fd);
        return "Log directory is not a private directory owned by the current user";
    }
    struct stat file_stat{};
    if (fstatat(fd, filename.c_str(), &file_stat, AT_SYMLINK_NOFOLLOW) != 0) {
        const std::string error = std::string("Cannot inspect log: ") + std::strerror(errno);
        close(fd);
        return error;
    }
    if (!S_ISREG(file_stat.st_mode)) {
        close(fd);
        return "Log is not a regular file";
    }
    if (unlinkat(fd, filename.c_str(), 0) != 0) {
        const std::string error = std::string("Cannot delete log: ") + std::strerror(errno);
        close(fd);
        return error;
    }
    close(fd);
    return {};
#else
    std::error_code failure;
    const auto path = directory / std::filesystem::u8path(filename);
    const auto status = std::filesystem::symlink_status(path, failure);
    if (failure) return "Cannot inspect log: " + failure.message();
    if (!std::filesystem::is_regular_file(status)) return "Log is not a regular file";
#if defined(_WIN32)
    const DWORD file_attributes = GetFileAttributesW(path.c_str());
    if (file_attributes == INVALID_FILE_ATTRIBUTES) return "Cannot inspect log attributes";
    if (file_attributes & FILE_ATTRIBUTE_REPARSE_POINT) return "Log is a reparse point";
#endif
    if (!std::filesystem::remove(path, failure))
        return "Cannot delete log: " + (failure ? failure.message() : "file not found");
    return {};
#endif
}
} // namespace

EvidenceFileList list_evidence_files(const std::filesystem::path& directory) {
    EvidenceFileList result;
    bool missing = false;
    result.error = security_directory_error(directory, missing);
    if (!result.error.empty() || missing) return result;
    std::error_code failure;
    std::size_t visited = 0;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(directory, failure);
         !failure && it != end; it.increment(failure)) {
        if (++visited > 4096) { result.error = "Too many directory entries (limit 4096)"; break; }
        const auto name = it->path().filename().string();
        if (!valid_export_name(name)) continue;
        const auto status = it->symlink_status(failure);
        if (failure) break;
        if (!std::filesystem::is_regular_file(status)) continue;
#if defined(_WIN32)
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            result.error = "Cannot inspect evidence attributes";
            break;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
#endif
        if (result.names.size() == 512) {
            result.error = "Too many evidence files (limit 512)";
            break;
        }
        result.names.push_back(name);
    }
    if (failure) result.error = "Cannot list evidence files: " + failure.message();
    if (!result.error.empty()) result.names.clear();
    else std::sort(result.names.begin(), result.names.end(), std::greater<std::string>());
    return result;
}

std::string delete_evidence_file(const std::filesystem::path& directory, const std::string& filename) {
    if (!valid_export_name(filename)) return "Invalid evidence file name";
    return delete_managed_file(directory, filename);
}

std::string delete_all_evidence_files(const std::filesystem::path& directory) {
    const auto files = list_evidence_files(directory);
    if (!files.error.empty()) return files.error;
    for (const auto& name : files.names) {
        const auto error = delete_evidence_file(directory, name);
        if (!error.empty()) return "Deletion stopped at " + name + ": " + error;
    }
    return {};
}

EvidenceExportResult export_evidence(const std::filesystem::path& source_directory,
                                     const std::string& filename,
                                     const std::filesystem::path& destination,
                                     EvidenceSource source) {
    EvidenceExportResult result;
    if (!valid_source_name(filename, source)) {
        result.error = "Invalid evidence source name";
        return result;
    }
#if !defined(_WIN32) && USE_DESKTOP
    result.error = "Evidence export is unavailable on this desktop";
    return result;
#else
    const auto source_path = source_directory / std::filesystem::u8path(filename);
    const std::size_t limit = source == EvidenceSource::Survey ? 10 * 1024 * 1024 : 1024 * 1024;
    result.error = ensure_private_directory(destination);
    if (!result.error.empty()) return result;
    bool destination_missing = false;
    result.error = security_directory_error(destination, destination_missing);
    if (!result.error.empty()) return result;
    if (destination_missing) { result.error = "Evidence directory not found"; return result; }
#if defined(_WIN32)
    HANDLE input = CreateFileW(source_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (input == INVALID_HANDLE_VALUE) { result.error = "Cannot open evidence source"; return result; }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileType(input) == FILE_TYPE_DISK && GetFileInformationByHandle(input, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow <= limit;
    if (!valid) { CloseHandle(input); result.error = "Evidence source is not a bounded regular file"; return result; }
#else
    const int input = open(source_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) { result.error = std::string("Cannot open evidence source: ") + std::strerror(errno); return result; }
    struct stat status{};
    if (fstat(input, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::size_t>(status.st_size) > limit) {
        close(input); result.error = "Evidence source is not a bounded regular file"; return result;
    }
#endif
    Hash hash;
    auto copy = open_unique_file(destination, "evidence-copy-", ".bin");
    if (!copy.error.empty() || !hash.start()) {
        result.error = copy.error.empty() ? "Cannot initialize SHA-256" : copy.error;
        if (copy.stream) { copy.stream.reset(); result.error += remove_incomplete_file(copy.path); }
#if defined(_WIN32)
        CloseHandle(input);
#else
        close(input);
#endif
        return result;
    }
    std::array<unsigned char, 8192> buffer{};
    std::size_t total = 0;
    for (;;) {
#if defined(_WIN32)
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            result.error = "Cannot read evidence source"; break;
        }
#else
        const auto count = read(input, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result.error = std::string("Cannot read evidence source: ") + std::strerror(errno); break; }
#endif
        if (!count) break;
        total += static_cast<std::size_t>(count);
        if (total > limit || !hash.update(buffer.data(), static_cast<std::size_t>(count)) ||
            std::fwrite(buffer.data(), 1, static_cast<std::size_t>(count), copy.stream.get()) !=
                static_cast<std::size_t>(count)) {
            result.error = "Evidence copy failed or source exceeded size limit"; break;
        }
    }
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION final_info{};
    if (result.error.empty() &&
        (!GetFileInformationByHandle(input, &final_info) ||
         final_info.nFileSizeHigh != info.nFileSizeHigh ||
         final_info.nFileSizeLow != info.nFileSizeLow ||
         CompareFileTime(&final_info.ftLastWriteTime, &info.ftLastWriteTime) != 0 ||
         total != info.nFileSizeLow)) {
        result.error = "Evidence source changed while copying";
    }
    CloseHandle(input);
#else
    struct stat final_status{};
    if (result.error.empty() &&
        (fstat(input, &final_status) != 0 ||
         final_status.st_size != status.st_size ||
         final_status.st_mtim.tv_sec != status.st_mtim.tv_sec ||
         final_status.st_mtim.tv_nsec != status.st_mtim.tv_nsec ||
         final_status.st_ctim.tv_sec != status.st_ctim.tv_sec ||
         final_status.st_ctim.tv_nsec != status.st_ctim.tv_nsec ||
         total != static_cast<std::size_t>(status.st_size))) {
        result.error = "Evidence source changed while copying";
    }
    close(input);
#endif
    const auto digest = result.error.empty() ? hash.finish() : std::string{};
    if (digest.empty() && result.error.empty()) result.error = "Cannot finish SHA-256";
    if (std::fclose(copy.stream.release()) != 0 && result.error.empty()) result.error = "Cannot close evidence copy";
    if (!result.error.empty()) { result.error += remove_incomplete_file(copy.path); return result; }
    auto manifest = open_unique_file(destination, "evidence-manifest-", ".txt");
    if (!manifest.error.empty()) {
        result.error = manifest.error + remove_incomplete_file(copy.path);
        return result;
    }
    const auto now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    const bool time_ok = now != static_cast<std::time_t>(-1) && gmtime_s(&utc, &now) == 0;
#else
    const bool time_ok = now != static_cast<std::time_t>(-1) && gmtime_r(&now, &utc);
#endif
    char timestamp[32]{};
    const bool ok = time_ok && std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) &&
        std::fprintf(manifest.stream.get(),
            "POSEIDON evidence export\nExported UTC (system clock): %s\nSource type: %s\n"
            "Source file: %s\nCopy file: %s\nBytes: %zu\nSHA-256: %s\n"
            "Hash covers the copy; it does not prove source authenticity or custody.\n",
            timestamp, source == EvidenceSource::Inventory ? "inventory" :
                       source == EvidenceSource::Survey ? "survey" :
                       source == EvidenceSource::LabListener ? "lab-listener" :
                       source == EvidenceSource::DeauthMonitor ? "deauth-monitor" :
                       source == EvidenceSource::ProbeMonitor ? "probe-monitor" : "adsb",
            filename.c_str(), copy.path.filename().u8string().c_str(), total, digest.c_str()) >= 0;
    const bool closed = std::fclose(manifest.stream.release()) == 0;
    if (!ok || !closed) {
        result.error = "Cannot write evidence manifest" + remove_incomplete_file(manifest.path) +
                       remove_incomplete_file(copy.path);
        return result;
    }
    result.copy_path = copy.path;
    result.manifest_path = manifest.path;
    return result;
#endif
}

} // namespace platform
