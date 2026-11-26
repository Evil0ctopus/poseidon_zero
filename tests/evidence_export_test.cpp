#include "evidence_export.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("poseidon-evidence-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto source = root / "source";
    const auto destination = root / "exports";
    std::filesystem::create_directories(source);
    const std::string name = "poseidon-host-discovery-1-0.txt";
    {
        std::ofstream file(source / name, std::ios::binary);
        file << "abc";
    }
    std::atomic_bool cancelled{false};
    const auto digest = platform::hash_local_file(source / name, cancelled);
    if (!digest.error.empty() ||
        digest.sha256 != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ||
        platform::hash_local_file(source, cancelled).error.empty() ||
        platform::hash_local_file(source / "missing", cancelled).error.empty()) {
        std::cerr << "File hash or invalid file handling failed: " << digest.error << '\n';
        return 1;
    }
    cancelled = true;
    if (platform::hash_local_file(source / name, cancelled).error.empty()) {
        std::cerr << "Cancelled hash must not succeed\n";
        return 1;
    }
    cancelled = false;
    const auto empty_file = source / "empty-file";
    { std::ofstream file(empty_file, std::ios::binary); }
    const auto empty_digest = platform::hash_local_file(empty_file, cancelled);
    if (!empty_digest.error.empty() ||
        empty_digest.sha256 != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        std::cerr << "Empty file hash failed: " << empty_digest.error << '\n';
        return 1;
    }
    std::filesystem::remove(empty_file);
    const auto large_file = source / "large-file";
    {
        std::ofstream file(large_file, std::ios::binary);
        file.seekp(10 * 1024 * 1024);
        file.put('x');
    }
    if (platform::hash_local_file(large_file, cancelled).error.empty()) {
        std::cerr << "Oversized hash input must be rejected\n";
        return 1;
    }
    std::filesystem::remove(large_file);
    const auto result = platform::export_evidence(
        source, name, destination, platform::EvidenceSource::Inventory);
    std::ifstream copy(result.copy_path, std::ios::binary);
    std::ifstream manifest(result.manifest_path, std::ios::binary);
    const std::string copied{std::istreambuf_iterator<char>(copy), {}};
    const std::string details{std::istreambuf_iterator<char>(manifest), {}};
    copy.close();
    manifest.close();
    if (!result.error.empty() || copied != "abc" ||
        details.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ==
            std::string::npos ||
        details.find(name) == std::string::npos ||
        platform::export_evidence(source, "../outside.txt", destination,
                                  platform::EvidenceSource::Inventory).error.empty() ||
        platform::export_evidence(source, "survey-2-0.csv", destination,
                                  platform::EvidenceSource::Survey).error.empty()) {
        std::cerr << "Evidence export or SHA-256 validation failed: " << result.error << '\n';
        return 1;
    }
    const std::string survey = "survey--2-0.csv";
    {
        std::ofstream file(source / survey, std::ios::binary);
        file << "survey";
    }
    const auto survey_result = platform::export_evidence(
        source, survey, destination, platform::EvidenceSource::Survey);
    if (!survey_result.error.empty()) {
        std::cerr << "Survey evidence export failed: " << survey_result.error << '\n';
        return 1;
    }
    const std::string lab_log = "lab-listener--3-0.txt";
    {
        std::ofstream file(source / lab_log, std::ios::binary);
        file << "lab";
    }
    const auto lab_result = platform::export_evidence(
        source, lab_log, destination, platform::EvidenceSource::LabListener);
    std::ifstream lab_manifest(lab_result.manifest_path, std::ios::binary);
    const std::string lab_details{std::istreambuf_iterator<char>(lab_manifest), {}};
    lab_manifest.close();
    if (!lab_result.error.empty() || lab_details.find("lab-listener") == std::string::npos ||
        platform::export_evidence(source, "../lab-listener-3-0.txt", destination,
                                  platform::EvidenceSource::LabListener).error.empty()) {
        std::cerr << "Lab listener evidence export failed: " << lab_result.error << '\n';
        return 1;
    }
    const std::string deauth_log = "deauth-monitor-4-0.txt";
    {
        std::ofstream file(source / deauth_log, std::ios::binary);
        file << "observation";
    }
    const auto deauth_result = platform::export_evidence(
        source, deauth_log, destination, platform::EvidenceSource::DeauthMonitor);
    std::ifstream deauth_manifest(deauth_result.manifest_path, std::ios::binary);
    const std::string deauth_details{std::istreambuf_iterator<char>(deauth_manifest), {}};
    deauth_manifest.close();
    if (!deauth_result.error.empty() ||
        deauth_details.find("deauth-monitor") == std::string::npos) {
        std::cerr << "Deauth evidence export failed: " << deauth_result.error << '\n';
        return 1;
    }
    const std::string probe_log = "probe-monitor-5-0.txt";
    {
        std::ofstream file(source / probe_log, std::ios::binary);
        file << "observation";
    }
    const auto probe_result = platform::export_evidence(
        source, probe_log, destination, platform::EvidenceSource::ProbeMonitor);
    if (!probe_result.error.empty()) {
        std::cerr << "Probe evidence export failed: " << probe_result.error << '\n';
        return 1;
    }
    if (platform::delete_security_log(source, "../" + lab_log,
                                      platform::EvidenceSource::LabListener).empty() ||
        platform::delete_security_log(source, survey,
                                      platform::EvidenceSource::Survey).empty() ||
        platform::delete_security_log(source, lab_log,
                                      platform::EvidenceSource::LabListener) != "" ||
        std::filesystem::exists(source / lab_log) ||
        !std::filesystem::exists(lab_result.copy_path) ||
        !std::filesystem::exists(lab_result.manifest_path) ||
        platform::delete_security_log(source, lab_log,
                                      platform::EvidenceSource::LabListener).empty()) {
        std::cerr << "Security log deletion must be bounded and preserve exports\n";
        return 1;
    }
    if (!platform::list_security_logs(root).entries.empty() ||
        !platform::list_security_logs(root).error.empty()) {
        std::cerr << "Missing security log directories should list as empty\n";
        return 1;
    }
    const auto lab_directory = root / "poseidon-lab";
    const auto wireless_directory = root / "poseidon-wireless-monitor";
    std::filesystem::create_directory(lab_directory);
    std::filesystem::create_directory(wireless_directory);
    { std::ofstream file(lab_directory / lab_log); file << "lab"; }
    { std::ofstream file(wireless_directory / deauth_log); file << "deauth"; }
    { std::ofstream file(wireless_directory / probe_log); file << "probe"; }
    { std::ofstream file(lab_directory / "unrelated.txt"); file << "unrelated"; }
    const auto logs = platform::list_security_logs(root);
    if (!logs.error.empty() || logs.entries.size() != 3 ||
        logs.entries[0].filename != probe_log ||
        logs.entries[0].source != platform::EvidenceSource::ProbeMonitor ||
        logs.entries[1].filename != lab_log ||
        logs.entries[1].source != platform::EvidenceSource::LabListener ||
        logs.entries[2].filename != deauth_log ||
        logs.entries[2].source != platform::EvidenceSource::DeauthMonitor ||
        platform::security_log_directory(root, platform::EvidenceSource::Survey) !=
            std::filesystem::path{} ||
        platform::delete_security_log(wireless_directory, deauth_log,
                                      platform::EvidenceSource::ProbeMonitor).empty()) {
        std::cerr << "Security log listing or source validation failed: " << logs.error << '\n';
        return 1;
    }
    const auto adsb_directory = root / "poseidon-adsb";
    const std::string adsb_log = "adsb-receiver--10-0.csv";
    std::filesystem::create_directory(adsb_directory);
    { std::ofstream file(adsb_directory / adsb_log); file << "address\n40621D\n"; }
    const auto adsb_logs = platform::list_security_logs(root);
    const auto adsb_export = platform::export_evidence(
        adsb_directory, adsb_log, destination, platform::EvidenceSource::Adsb);
    if (!adsb_logs.error.empty() || adsb_logs.entries.size() != 4 ||
        adsb_logs.entries.back().filename != adsb_log ||
        adsb_logs.entries.back().source != platform::EvidenceSource::Adsb ||
        !adsb_export.error.empty() ||
        !platform::delete_security_log(adsb_directory, adsb_log, platform::EvidenceSource::Adsb).empty() ||
        std::filesystem::exists(adsb_directory / adsb_log) ||
        !std::filesystem::exists(adsb_export.copy_path)) {
        std::cerr << "ADS-B log listing, export or deletion failed: " << adsb_export.error << '\n';
        return 1;
    }
    std::filesystem::remove(adsb_export.copy_path);
    std::filesystem::remove(adsb_export.manifest_path);
    std::filesystem::remove(adsb_directory);
    const auto link_name = "lab-listener-9-0.txt";
    std::error_code link_error;
    std::filesystem::create_symlink(lab_directory / lab_log, lab_directory / link_name, link_error);
#if !defined(_WIN32)
    if (link_error) {
        std::cerr << "Could not create symlink fixture: " << link_error.message() << '\n';
        return 1;
    }
#endif
    if (!link_error) {
        const auto with_link = platform::list_security_logs(root);
        if (!with_link.error.empty() || with_link.entries.size() != 3 ||
            platform::hash_local_file(lab_directory / link_name, cancelled).error.empty() ||
            platform::delete_security_log(lab_directory, link_name,
                                          platform::EvidenceSource::LabListener).empty() ||
            !std::filesystem::exists(lab_directory / lab_log)) {
            std::cerr << "Security log symlink must be ignored and rejected\n";
            return 1;
        }
        std::filesystem::remove(lab_directory / link_name);
    }
#if !defined(_WIN32)
    const auto directory_link = root / "directory-link";
    std::filesystem::create_directory_symlink(lab_directory, directory_link, link_error);
    if (link_error ||
        platform::delete_security_log(directory_link, lab_log,
                                      platform::EvidenceSource::LabListener).empty() ||
        !std::filesystem::exists(lab_directory / lab_log)) {
        std::cerr << "Security log directory symlinks must be rejected\n";
        return 1;
    }
    std::filesystem::remove(directory_link);
#endif
    std::filesystem::remove(lab_directory / lab_log);
    std::filesystem::remove(lab_directory / "unrelated.txt");
    std::filesystem::remove(wireless_directory / deauth_log);
    std::filesystem::remove(wireless_directory / probe_log);
    std::filesystem::remove(lab_directory);
    std::filesystem::remove(wireless_directory);
    const std::string oversized = "poseidon-host-discovery-3-0.txt";
    {
        std::ofstream file(source / oversized, std::ios::binary);
        file.seekp(1024 * 1024);
        file.put('x');
    }
    if (platform::export_evidence(source, oversized, destination,
                                  platform::EvidenceSource::Inventory).error.empty()) {
        std::cerr << "Oversized evidence must be rejected\n";
        return 1;
    }
    { std::ofstream file(destination / "keep.txt"); file << "keep"; }
    { std::ofstream file(destination / "evidence-copy--123-0.bin"); file << "early clock"; }
    { std::ofstream file(destination / "survey-gps-123-0.gpx"); file << "GPS"; }
    { std::ofstream file(destination / "settings-backup-123-0.ini"); file << "settings"; }
    const auto exports = platform::list_evidence_files(destination);
    if (!exports.error.empty() || exports.names.size() != 13 ||
        platform::delete_evidence_file(destination, "../keep.txt").empty() ||
        platform::delete_evidence_file(destination, "keep.txt").empty() ||
        platform::delete_evidence_file(destination, "evidence-copy---123-0.bin").empty() ||
        !platform::delete_evidence_file(destination, result.copy_path.filename().string()).empty() ||
        std::filesystem::exists(result.copy_path) || !std::filesystem::exists(result.manifest_path)) {
        std::cerr << "Evidence file listing or selective deletion failed: " << exports.error << '\n';
        return 1;
    }
#if !defined(_WIN32)
    const auto export_link = destination / "evidence-copy-999-0.bin";
    std::filesystem::create_symlink(destination / "keep.txt", export_link, link_error);
    if (link_error || platform::list_evidence_files(destination).names.size() != 12 ||
        platform::delete_evidence_file(destination, export_link.filename().string()).empty()) {
        std::cerr << "Evidence file symlink was accepted\n";
        return 1;
    }
    std::filesystem::remove(export_link);
#endif
    const auto delete_error = platform::delete_all_evidence_files(destination);
    if (!delete_error.empty() || !platform::list_evidence_files(destination).names.empty() ||
        !std::filesystem::exists(destination / "keep.txt") || !std::filesystem::exists(source / name)) {
        std::cerr << "Bulk evidence deletion failed or affected other files: " << delete_error << '\n';
        return 1;
    }
    std::filesystem::remove(destination / "keep.txt");
    std::filesystem::remove(source / name);
    std::filesystem::remove(source / survey);
    std::filesystem::remove(source / lab_log);
    std::filesystem::remove(source / deauth_log);
    std::filesystem::remove(source / probe_log);
    std::filesystem::remove(source / oversized);
    std::filesystem::remove(source);
    std::filesystem::remove(destination);
    std::filesystem::remove(root);
    return 0;
}
