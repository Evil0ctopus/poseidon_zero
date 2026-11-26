#pragma once

#include <filesystem>
#include <atomic>
#include <string>
#include <vector>

namespace platform {

enum class EvidenceSource { Inventory, Survey, LabListener, DeauthMonitor, ProbeMonitor, Adsb };

struct EvidenceExportResult {
    std::filesystem::path copy_path;
    std::filesystem::path manifest_path;
    std::string error;
};

struct FileHashResult {
    std::string sha256;
    std::string error;
};

FileHashResult hash_local_file(const std::filesystem::path& path, const std::atomic_bool& cancelled);

struct SecurityLogEntry {
    std::string filename;
    EvidenceSource source{EvidenceSource::LabListener};
};

struct SecurityLogList {
    std::vector<SecurityLogEntry> entries;
    std::string error;
};

struct EvidenceFileList {
    std::vector<std::string> names;
    std::string error;
};

EvidenceFileList list_evidence_files(const std::filesystem::path& directory);
std::string delete_evidence_file(const std::filesystem::path& directory, const std::string& filename);
std::string delete_all_evidence_files(const std::filesystem::path& directory);

SecurityLogList list_security_logs(const std::filesystem::path& home);
std::filesystem::path security_log_directory(const std::filesystem::path& home,
                                             EvidenceSource source);

EvidenceExportResult export_evidence(const std::filesystem::path& source_directory,
                                     const std::string& filename,
                                     const std::filesystem::path& destination,
                                     EvidenceSource source);

std::string delete_security_log(const std::filesystem::path& directory,
                                const std::string& filename,
                                EvidenceSource source);

} // namespace platform
