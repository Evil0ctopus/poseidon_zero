#pragma once

#include "adsb.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform {

inline constexpr char kAdsbReceiverExecutable[] = "/usr/bin/rtl_adsb";
inline constexpr std::size_t kAdsbRecentAircraftLimit = 32;

struct AdsbReceiverStatus {
    bool running{false};
    bool complete{false};
    unsigned valid_frames{0};
    unsigned rejected_frames{0};
    AdsbFrame latest;
    std::vector<AdsbFrame> recent_aircraft;
    std::string log_filename;
    std::string gps_status;
    std::string error;
};

class AdsbReceiver {
public:
    static std::unique_ptr<AdsbReceiver> start(std::string& error,
                                               const std::filesystem::path& log_home = {},
                                               const std::string& gps_device = {});
#if defined(POSEIDON_TEST_ADSB)
    static std::unique_ptr<AdsbReceiver> start_fixture(const std::string& executable, std::string& error,
                                                       const std::filesystem::path& log_home = {},
                                                       const std::string& gps_device = {});
#endif
    ~AdsbReceiver();
    void stop();
    AdsbReceiverStatus status() const;

    AdsbReceiver(const AdsbReceiver&) = delete;
    AdsbReceiver& operator=(const AdsbReceiver&) = delete;

private:
    AdsbReceiver() = default;
    static std::unique_ptr<AdsbReceiver> start_with(const std::string& executable, std::string& error,
                                                    const std::filesystem::path& log_home,
                                                    const std::string& gps_device);
    void run(std::string executable, std::filesystem::path log_home, std::string gps_device);
    void run_impl(const std::string& executable, const std::filesystem::path& log_home,
                  const std::string& gps_device);
    std::atomic_bool cancelled_{false};
    mutable std::mutex mutex_;
    AdsbReceiverStatus status_;
    std::thread thread_;
};

} // namespace platform
