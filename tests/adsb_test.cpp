#include "adsb.h"

#include <chrono>
#include <cmath>
#include <iostream>

int main() {
    const auto position = platform::decode_adsb_frame("8D40621D58C382D690C8AC2863A7");
    if (!position.error.empty() || position.aircraft_address != "40621D" ||
        position.type_code != 11 || !position.altitude_available || position.altitude_feet != 38000) {
        std::cerr << "Valid ADS-B position frame failed: " << position.error << '\n';
        return 1;
    }
    const auto identity = platform::decode_adsb_frame("8d4840d6202cc371c32ce0576098");
    if (!identity.error.empty() || identity.aircraft_address != "4840D6" || identity.callsign != "KLM1023" ||
        identity.altitude_available || identity.emitter_category != 0) {
        std::cerr << "Valid ADS-B identification frame failed: " << identity.error << '\n';
        return 1;
    }
    const auto unmanned = platform::decode_adsb_frame("8D4840D61E2CC371C32CE035A441");
    const auto rotorcraft = platform::decode_adsb_frame("8D4840D6272CC371C32CE0421846");
    const auto ground_obstacle = platform::decode_adsb_frame("8D4840D6152CC371C32CE04D2CC4");
    if (!unmanned.error.empty() || unmanned.type_code != 3 || unmanned.emitter_category != 6 ||
        !rotorcraft.error.empty() || rotorcraft.type_code != 4 || rotorcraft.emitter_category != 7 ||
        !ground_obstacle.error.empty() || ground_obstacle.type_code != 2 || ground_obstacle.emitter_category != 5 ||
        position.emitter_category != -1) {
        std::cerr << "ADS-B emitter category decoding failed\n";
        return 1;
    }
    const auto normal_status = platform::decode_adsb_frame("8DA2C1B6E112B600000000760759");
    const auto emergency = platform::decode_adsb_frame("8DA2C1B6E12AAA000000000D81F4");
    const auto reserved = platform::decode_adsb_frame("8DA2C1B6E1E04000000000AF3D0C");
    const auto zero_code = platform::decode_adsb_frame("8DA2C1B6E1000000000000EF2691");
    const auto common_code = platform::decode_adsb_frame("8DA2C1B6E1080800000000A19055");
    const auto other_status = platform::decode_adsb_frame("8DA2C1B6E22808000000009E9D9C");
    if (!normal_status.error.empty() || !normal_status.emergency_status_available ||
        normal_status.emergency_status != 0 || normal_status.squawk != "6513" ||
        !emergency.error.empty() || !emergency.emergency_status_available ||
        emergency.emergency_status != 1 || emergency.squawk != "7700" ||
        !reserved.error.empty() || !reserved.emergency_status_available ||
        reserved.emergency_status != 7 || !reserved.squawk.empty() ||
        !zero_code.error.empty() || zero_code.squawk != "0000" ||
        !common_code.error.empty() || common_code.squawk != "1200" ||
        !other_status.error.empty() || other_status.emergency_status_available || !other_status.squawk.empty()) {
        std::cerr << "ADS-B emergency status or squawk decoding failed\n";
        return 1;
    }
    const auto unavailable = platform::decode_adsb_frame("8D40621D580002D690C8AC94B055");
    const auto gillham = platform::decode_adsb_frame("8D40621D58C282D690C8ACDD45B5");
    if (!unavailable.error.empty() || unavailable.altitude_available ||
        !gillham.error.empty() || gillham.altitude_available) {
        std::cerr << "Unsupported or unavailable altitude must not be reported\n";
        return 1;
    }
    if (platform::decode_adsb_frame("8D40621D58C382D690C8AC2863A6").error.empty() ||
        platform::decode_adsb_frame("8D40621D58C382D690C8AC2863AZ").error.empty() ||
        platform::decode_adsb_frame("8D40621D").error.empty() ||
        platform::decode_adsb_frame("5D40621D58C382D690C8AC2863A7").error.empty()) {
        std::cerr << "Malformed or corrupt ADS-B frame was accepted\n";
        return 1;
    }
    const auto velocity = platform::decode_adsb_frame("8D485020994409940838175B284F");
    const auto airspeed = platform::decode_adsb_frame("8DA05F219B06B6AF189400CBC33F");
    if (!velocity.error.empty() || velocity.speed_type != platform::AdsbSpeedType::Ground ||
        !velocity.speed_available || std::abs(velocity.speed_knots - 159.20) > 0.02 ||
        !velocity.direction_available || std::abs(velocity.direction_degrees - 182.88) > 0.02 ||
        !velocity.vertical_rate_available || velocity.vertical_rate_barometric ||
        velocity.vertical_rate_fpm != -832 ||
        !airspeed.error.empty() || airspeed.speed_type != platform::AdsbSpeedType::TrueAirspeed ||
        !airspeed.speed_available || airspeed.speed_knots != 375 ||
        !airspeed.direction_available || std::abs(airspeed.direction_degrees - 243.984375) > 0.001 ||
        !airspeed.vertical_rate_available || !airspeed.vertical_rate_barometric ||
        airspeed.vertical_rate_fpm != -2304) {
        std::cerr << "ADS-B velocity decoding failed\n";
        return 1;
    }
    const auto missing_axis = platform::decode_adsb_frame("8D485020994400940838174074F1");
    const auto stationary = platform::decode_adsb_frame("8D48502099440180283817163E71");
    const auto fast = platform::decode_adsb_frame("8D4850209A440994083817C0535F");
    const auto no_vertical = platform::decode_adsb_frame("8D48502099440994080017F5D846");
    const auto no_heading = platform::decode_adsb_frame("8DA05F219B02B680189400133912");
    const auto indicated = platform::decode_adsb_frame("8DA05F219C06B62F189400DF3E86");
    if (!missing_axis.error.empty() || missing_axis.speed_available || missing_axis.direction_available ||
        !stationary.error.empty() || !stationary.speed_available || stationary.speed_knots != 0 ||
        stationary.direction_available || !fast.error.empty() || !fast.speed_available ||
        std::abs(fast.speed_knots - velocity.speed_knots * 4) > 0.001 ||
        !no_vertical.error.empty() || no_vertical.vertical_rate_available ||
        !no_heading.error.empty() || no_heading.speed_available || no_heading.direction_available ||
        !indicated.error.empty() || indicated.speed_type != platform::AdsbSpeedType::IndicatedAirspeed ||
        !indicated.speed_available || indicated.speed_knots != 1500) {
        std::cerr << "ADS-B velocity edge cases failed\n";
        return 1;
    }
    using namespace std::chrono;
    const auto now = steady_clock::time_point{} + seconds(20);
    const auto odd = platform::decode_adsb_frame("8D40621D58C386435CC412692AD6");
    platform::AdsbPositionDecoder decoder;
    const auto first = decoder.update(position, now);
    const auto odd_result = decoder.update(odd, now + seconds(1));
    if (first.position_available || !odd_result.position_available ||
        std::abs(odd_result.latitude - 52.2657802) > 0.0001 ||
        std::abs(odd_result.longitude - 3.9389125) > 0.0001) {
        std::cerr << "Odd CPR position failed\n";
        return 1;
    }
    const auto even_result = decoder.update(position, now + seconds(2));
    if (!even_result.position_available ||
        std::abs(even_result.latitude - 52.2572021) > 0.0001 ||
        std::abs(even_result.longitude - 3.9193726) > 0.0001) {
        std::cerr << "Even CPR position failed\n";
        return 1;
    }
    platform::AdsbPositionDecoder stale;
    stale.update(position, now);
    if (stale.update(odd, now + seconds(11)).position_available) {
        std::cerr << "Stale CPR pair was accepted\n";
        return 1;
    }
    platform::AdsbPositionDecoder boundary;
    boundary.update(position, now);
    if (!boundary.update(odd, now + seconds(10)).position_available) {
        std::cerr << "Ten-second CPR pair was rejected\n";
        return 1;
    }
    platform::AdsbPositionDecoder different;
    auto other = odd;
    other.aircraft_address = "ABCDEF";
    different.update(position, now);
    if (different.update(other, now + seconds(1)).position_available ||
        different.update(odd, now - seconds(1)).position_available) {
        std::cerr << "Wrong aircraft or backward time was paired\n";
        return 1;
    }
    const auto us_even = platform::decode_adsb_frame("8D40621D581503EB86FD451C4E2B");
    const auto us_odd = platform::decode_adsb_frame("8D40621D58150785776D5424E6F2");
    platform::AdsbPositionDecoder west;
    west.update(us_odd, now);
    const auto us = west.update(us_even, now + seconds(1));
    if (!us.error.empty() || !us.position_available ||
        std::abs(us.latitude - 35.8800201) > 0.0001 ||
        std::abs(us.longitude - (-78.7899971)) > 0.0001) {
        std::cerr << "Western CPR position failed\n";
        return 1;
    }
    auto south_even = position;
    south_even.cpr_latitude = 45875;
    south_even.cpr_longitude = 76022;
    auto south_odd = odd;
    south_odd.cpr_latitude = 58218;
    south_odd.cpr_longitude = 20972;
    platform::AdsbPositionDecoder south;
    south.update(south_even, now);
    const auto southern = south.update(south_odd, now + seconds(1));
    if (!southern.position_available || std::abs(southern.latitude - (-33.9)) > 0.0001 ||
        std::abs(southern.longitude - 151.2) > 0.0001) {
        std::cerr << "Southern CPR position failed\n";
        return 1;
    }
    auto zone_even = position;
    zone_even.cpr_latitude = 114227;
    zone_even.cpr_longitude = 36150;
    auto zone_odd = odd;
    zone_odd.cpr_latitude = 101405;
    zone_odd.cpr_longitude = 93524;
    platform::AdsbPositionDecoder zone_boundary;
    zone_boundary.update(zone_even, now);
    if (zone_boundary.update(zone_odd, now + seconds(1)).position_available) {
        std::cerr << "CPR pair across longitude zones was accepted\n";
        return 1;
    }
    platform::AdsbPositionDecoder bounded;
    bounded.update(position, now);
    auto extra = position;
    for (int i = 0; i < 128; ++i) {
        extra.aircraft_address = "test-" + std::to_string(i);
        bounded.update(extra, now + milliseconds(i + 1));
    }
    if (bounded.update(odd, now + milliseconds(129)).position_available) {
        std::cerr << "Evicted CPR sample was retained\n";
        return 1;
    }
    return 0;
}
