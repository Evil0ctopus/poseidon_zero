#pragma once

#include "gnss.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform {

struct SurveillanceHint {
    std::string transport;
    std::string name;
    std::string address;
};

struct SurveillanceScanResult {
    std::vector<SurveillanceHint> hints;
    std::size_t stale_wifi{0};
    bool results_may_be_truncated{false};
    std::string error;
};

SurveillanceScanResult scan_surveillance_hints(const std::atomic_bool& cancelled);

struct SurveyStatus {
    bool complete{false};
    std::size_t wifi_count{0};
    std::size_t ble_count{0};
    std::size_t wifi_skipped{0};
    std::size_t gps_tagged{0};
    std::size_t gps_untagged{0};
    std::size_t indicator_count{0};
    std::size_t watchlist_rows{0};
    bool results_may_be_truncated{false};
    std::string gps_status{"Starting"};
    std::string phase{"Starting"};
    std::string file_name;
    std::string error;
};

class SurveySession {
public:
    static std::unique_ptr<SurveySession> start(std::filesystem::path directory,
                                                 std::string gps_device,
                                                 std::string& error);
    ~SurveySession();
    SurveySession(const SurveySession&) = delete;
    SurveySession& operator=(const SurveySession&) = delete;
    void stop();
    SurveyStatus status() const;

private:
    SurveySession() = default;
    void run(std::filesystem::path directory, std::string gps_device);
    mutable std::mutex mutex_;
    SurveyStatus status_;
    GnssFix fix_;
    bool has_fix_{false};
    std::atomic_bool cancelled_{false};
    std::thread thread_;
};

} // namespace platform
