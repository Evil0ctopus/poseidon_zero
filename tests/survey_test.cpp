#include "bluetooth_gatt.h"
#include "gnss.h"
#include "survey.h"
#include "survey_file.h"
#include "wifi_scan.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace platform {

std::atomic_bool fail_wifi{false};

bool gnss_fix_is_fresh(const GnssFix& fix, std::chrono::system_clock::time_point now) {
    return now >= fix.utc && now - fix.utc <= std::chrono::seconds(5);
}
GnssReceiver::GnssReceiver(int fd) : fd_(fd) {}
std::unique_ptr<GnssReceiver> GnssReceiver::open(const std::string&, std::string&) {
    return std::unique_ptr<GnssReceiver>(new GnssReceiver(-1));
}
GnssReceiver::~GnssReceiver() = default;
bool GnssReceiver::poll(GnssFix& fix, bool& has_fix, std::string&) {
    fix = {38.9, -92.3, std::chrono::system_clock::now(), "A"};
    has_fix = true;
    return true;
}

WifiScanResult scan_wifi(const std::atomic_bool&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    WifiScanResult result;
    if (fail_wifi) {
        result.error = "mock scan failed";
        return result;
    }
    result.scan_started_boottime = 100;
    WifiScanNetwork fresh;
    fresh.name = "Camera Tracker";
    fresh.address = "11:22:33:44:55:66";
    fresh.strength = 42;
    fresh.frequency_mhz = 2412;
    fresh.last_seen_boottime = 101;
    WifiScanNetwork stale = fresh;
    stale.name = "stale AP";
    stale.last_seen_boottime = 90;
    result.networks = {fresh, stale};
    return result;
}
bool wifi_seen_during_scan(const WifiScanResult& result, const WifiScanNetwork& network) {
    return network.last_seen_boottime > result.scan_started_boottime &&
           network.last_seen_boottime >= 0;
}
int wifi_channel(uint32_t frequency) { return frequency == 2412 ? 1 : -1; }
BluetoothScanResult scan_ble_devices(const std::atomic_bool&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    BluetoothDevice drone{"/device", "AA:BB:CC:DD:EE:FF", "Flipper Cardputer", true, {}, true};
    drone.remote_id.aircraft_id = "AIRCRAFT-123";
    drone.remote_id.aircraft_id_type = "serial";
    drone.remote_id.id_observed_at = std::chrono::system_clock::now();
    drone.remote_id.position = RemoteIdPosition{37.2, -93.1, std::chrono::system_clock::now()};
    return {{drone, {"/other", "11:22:33:44:55:77", "ordinary device", false, {}, false},
             {"/third", "11:22:33:44:55:88", "DRONE Skimmer", false, {}, false},
             {"/fourth", "11:22:33:44:55:99", "BodyCam", false, {}, false}}, {}};
}

} // namespace platform

int main() {
    using namespace platform;
    namespace fs = std::filesystem;
    std::atomic_bool cancelled{false};
    const auto hints = scan_surveillance_hints(cancelled);
    if (!hints.error.empty() || hints.hints.size() != 2 || hints.stale_wifi != 1 ||
        hints.hints[0].transport != "Wi-Fi" || hints.hints[0].name != "Camera Tracker" ||
        hints.hints[1].transport != "BLE" || hints.hints[1].name != "BodyCam") {
        std::cerr << "Surveillance name hints must use fresh observations: " << hints.error << '\n';
        return 1;
    }
    fail_wifi = true;
    if (scan_surveillance_hints(cancelled).error.find("mock scan failed") == std::string::npos) {
        std::cerr << "Surveillance scan errors must be reported\n";
        return 1;
    }
    fail_wifi = false;
    const auto directory = fs::temp_directory_path() /
        ("poseidon-survey-worker-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
    const auto indicator_file = directory / "name-indicators.txt";
    {
        std::ofstream indicators(indicator_file);
        indicators << "# locally supplied patterns\nCaMeRa\n";
    }
    std::string error;
    auto session = SurveySession::start(directory, "", error);
    if (!session) {
        std::cerr << "Cannot start mock session: " << error << '\n';
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session->status().ble_count < 3 && !session->status().complete &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    session->stop();
    while (!session->status().complete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto status = session->status();
    session.reset();
    bool ok = status.complete && status.error.empty() && status.wifi_count > 0 &&
              status.ble_count >= 3 && status.wifi_skipped > 0 && status.indicator_count >= 3 &&
              status.watchlist_rows == status.wifi_count &&
              status.gps_tagged == 0 &&
              status.gps_untagged == status.wifi_count + status.ble_count &&
              status.gps_status == "GPS device not configured";
    std::ifstream input(directory / status.file_name, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(input)), {});
    input.close();
    ok = ok && data.find("Camera Tracker") != std::string::npos &&
         data.find("stale AP") == std::string::npos &&
         data.find("Flipper Cardputer") != std::string::npos &&
         data.find("ordinary device") != std::string::npos &&
         data.find("name:drone;name:skimmer") != std::string::npos &&
         data.find("name:camera;name:tracker") != std::string::npos &&
         data.find("user-name:camera") != std::string::npos &&
         data.find("name:flipper;name:cardputer;ble:remote_id_fffa;ble:tracker_fcb2") !=
             std::string::npos &&
         data.find("\"AIRCRAFT-123\",\"serial\"") != std::string::npos &&
         data.find("37.2000000,-93.1000000") != std::string::npos &&
         data.find("\"seen_during_scan\",false,,,") != std::string::npos;
    const auto ordinary = data.find("\"ordinary device\"");
    const auto ordinary_end = data.find('\n', ordinary);
    ok = ok && ordinary != std::string::npos && ordinary_end != std::string::npos &&
         data.substr(ordinary, ordinary_end - ordinary).find("name:") == std::string::npos &&
         data.substr(ordinary, ordinary_end - ordinary).find("ble:tracker_fcb2") ==
             std::string::npos &&
         data.substr(ordinary, ordinary_end - ordinary).find("AIRCRAFT-123") ==
             std::string::npos;
    const auto observed = std::chrono::system_clock::now();
    RemoteIdData decoded;
    std::uint8_t basic[27]{0x0d, 0, 0x02, 0x12};
    ok = ok && !update_remote_id(decoded, nullptr, sizeof(basic), observed) &&
         !update_remote_id(decoded, basic, sizeof(basic) - 1, observed);
    const std::string aircraft_id = "AIRCRAFT-123";
    for (std::size_t i = 0; i < aircraft_id.size(); ++i)
        basic[4 + i] = static_cast<std::uint8_t>(aircraft_id[i]);
    std::uint8_t position[27]{0x0d, 0, 0x12};
    const auto write_le32 = [](std::uint8_t* out, std::int32_t value) {
        const auto bits = static_cast<std::uint32_t>(value);
        for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    };
    write_le32(position + 7, 372000000);
    write_le32(position + 11, -931000000);
    ok = ok && update_remote_id(decoded, basic, sizeof(basic), observed) &&
         decoded.aircraft_id == aircraft_id && decoded.aircraft_id_type == "serial" &&
         decoded.id_observed_at == observed &&
         update_remote_id(decoded, position, sizeof(position), observed) && decoded.position &&
         decoded.position->latitude == 37.2 && decoded.position->longitude == -93.1 &&
         decoded.position->observed_at == observed;
    std::uint8_t packed[80]{0x0d, 0, 0xf2, 25, 3};
    for (std::size_t i = 0; i < 25; ++i) {
        packed[5 + i] = basic[2 + i];
        packed[30 + i] = position[2 + i];
    }
    packed[55] = 0x42;
    for (std::size_t i = 57; i < sizeof(packed); ++i) packed[i] = 0x7f;
    RemoteIdData packed_data;
    ok = ok && update_remote_id(packed_data, packed, sizeof(packed), observed) &&
         packed_data.aircraft_id == aircraft_id && packed_data.position &&
         packed_data.position->latitude == 37.2 && packed_data.position->longitude == -93.1 &&
         !update_remote_id(packed_data, packed, sizeof(packed) - 1, observed);
    for (std::size_t i = 7; i < 15; ++i) position[i] = 0;
    ok = ok && update_remote_id(decoded, position, sizeof(position), observed) &&
         !decoded.position;
    write_le32(position + 7, 910000000);
    write_le32(position + 11, -931000000);
    ok = ok && update_remote_id(decoded, position, sizeof(position), observed) &&
         !decoded.position;
    basic[2] = 0x03;
    RemoteIdData unsupported_version;
    ok = ok && update_remote_id(unsupported_version, basic, sizeof(basic), observed) &&
         unsupported_version.aircraft_id.empty() && !unsupported_version.position;
    basic[2] = 0x52;
    ok = ok && update_remote_id(unsupported_version, basic, sizeof(basic), observed) &&
         unsupported_version.aircraft_id.empty() && !unsupported_version.position;
    if (!ok) std::cerr << "Mock survey failed: " << status.error << '\n';
    auto located = SurveySession::start(directory, "mock-gps", error);
    if (!located) {
        std::cerr << "Cannot start GPS test: " << error << '\n';
        ok = false;
    } else {
        const auto gps_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (located->status().ble_count < 3 && !located->status().complete &&
               std::chrono::steady_clock::now() < gps_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        located->stop();
        while (!located->status().complete && std::chrono::steady_clock::now() < gps_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto gps_status = located->status();
        located.reset();
        std::ifstream gps_input(directory / gps_status.file_name, std::ios::binary);
        const std::string gps_data((std::istreambuf_iterator<char>(gps_input)), {});
        gps_input.close();
        if (!gps_status.complete || !gps_status.error.empty() ||
            gps_status.gps_tagged == 0 ||
            gps_status.gps_tagged + gps_status.gps_untagged != gps_status.wifi_count + gps_status.ble_count ||
            gps_data.find("38.9000000,-92.3000000") == std::string::npos) {
            std::cerr << "Mock GPS fix was not saved: " << gps_status.error << '\n';
            ok = false;
        }
    }
    fail_wifi = true;
    auto failing = SurveySession::start(directory, "", error);
    if (!failing) {
        std::cerr << "Cannot start failure test: " << error << '\n';
        ok = false;
    } else {
        const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!failing->status().complete && std::chrono::steady_clock::now() < failure_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto failed_status = failing->status();
        ok &= failed_status.complete && failed_status.error.find("mock scan failed") !=
                                           std::string::npos;
        failing.reset();
    }
    fail_wifi = false;
    {
        std::ofstream indicators(indicator_file);
        indicators << "bad pattern\n";
    }
    const auto rejects_indicator_file = [&](const std::string& expected) {
        auto invalid = SurveySession::start(directory, "", error);
        if (!invalid) {
            std::cerr << "Cannot start indicator error test: " << error << '\n';
            return false;
        }
        const auto invalid_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!invalid->status().complete && std::chrono::steady_clock::now() < invalid_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto invalid_status = invalid->status();
        if (!invalid_status.complete || invalid_status.error.find(expected) == std::string::npos ||
            invalid_status.wifi_count != 0 || invalid_status.ble_count != 0) {
            std::cerr << "Invalid indicators did not stop the survey explicitly\n";
            return false;
        }
        return true;
    };
    ok &= rejects_indicator_file("Indicator file:");
    {
        std::ofstream indicators(indicator_file);
        indicators << std::string(4097, 'a');
    }
    ok &= rejects_indicator_file("4096");
#if !defined(_WIN32)
    fs::remove(indicator_file);
    const auto target_file = directory / "indicator-target.txt";
    { std::ofstream target(target_file); target << "camera\n"; }
    fs::create_symlink(target_file.filename(), indicator_file);
    ok &= rejects_indicator_file("regular file");
    fs::remove(indicator_file);
    fs::remove(target_file);
#endif
    fs::remove(indicator_file);
    fail_wifi = true;
    auto missing = SurveySession::start(directory, "", error);
    if (!missing) {
        std::cerr << "Cannot start missing-indicator test: " << error << '\n';
        ok = false;
    } else {
        const auto missing_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!missing->status().complete && std::chrono::steady_clock::now() < missing_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!missing->status().complete ||
            missing->status().error.find("mock scan failed") == std::string::npos) {
            std::cerr << "Missing optional indicators prevented scanning\n";
            ok = false;
        }
        missing.reset();
    }
    const auto deletion = delete_all_survey_files(directory);
    std::error_code cleanup;
    fs::remove(indicator_file, cleanup);
    fs::remove(directory, cleanup);
    if (!deletion.empty() || cleanup) {
        std::cerr << "Could not remove test session\n";
        ok = false;
    }
    return ok ? 0 : 1;
}
