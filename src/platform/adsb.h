#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

namespace platform {

enum class AdsbSpeedType { None, Ground, IndicatedAirspeed, TrueAirspeed };

struct AdsbFrame {
    std::string aircraft_address;
    std::string callsign;
    int type_code{0};
    int emitter_category{-1};
    bool altitude_available{false};
    int altitude_feet{0};
    bool airborne_cpr_available{false};
    bool cpr_odd{false};
    uint32_t cpr_latitude{0};
    uint32_t cpr_longitude{0};
    bool position_available{false};
    double latitude{0};
    double longitude{0};
    AdsbSpeedType speed_type{AdsbSpeedType::None};
    bool speed_available{false};
    double speed_knots{0};
    bool direction_available{false};
    double direction_degrees{0};
    bool vertical_rate_available{false};
    bool vertical_rate_barometric{false};
    int vertical_rate_fpm{0};
    bool emergency_status_available{false};
    uint8_t emergency_status{0};
    std::string squawk;
    std::string error;
};

AdsbFrame decode_adsb_frame(const std::string& hex);

class AdsbPositionDecoder {
public:
    AdsbFrame update(AdsbFrame frame, std::chrono::steady_clock::time_point received);

private:
    struct Sample {
        bool valid{false};
        uint32_t latitude{0};
        uint32_t longitude{0};
        std::chrono::steady_clock::time_point received{};
    };
    struct Aircraft {
        std::string address;
        Sample even;
        Sample odd;
        std::chrono::steady_clock::time_point last_received{};
    };
    std::vector<Aircraft> aircraft_;
};

} // namespace platform
