#include "adsb.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace platform {
namespace {

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

uint32_t syndrome(const std::array<uint8_t, 14>& bytes) {
    uint32_t remainder = 0;
    for (const auto byte : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool top = (remainder & 0x800000U) != 0;
            remainder = ((remainder << 1) | ((byte >> bit) & 1U)) & 0xffffffU;
            if (top) remainder ^= 0xfff409U;
        }
    }
    return remainder;
}

int cpr_mod(int value, int divisor) {
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

int longitude_zones(double latitude) {
    constexpr double pi = 3.14159265358979323846;
    latitude = std::abs(latitude);
    if (latitude >= 87.0) return latitude == 87.0 ? 2 : 1;
    if (latitude == 0.0) return 59;
    const double cosine = std::cos(latitude * pi / 180.0);
    const double argument = 1.0 - (1.0 - std::cos(pi / 30.0)) / (cosine * cosine);
    return static_cast<int>(std::floor(2.0 * pi / std::acos(std::max(-1.0, std::min(1.0, argument)))));
}

} // namespace

AdsbFrame decode_adsb_frame(const std::string& hex) {
    AdsbFrame result;
    if (hex.size() != 28) {
        result.error = "ADS-B frame must contain 28 hexadecimal digits";
        return result;
    }
    std::array<uint8_t, 14> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const int high = hex_digit(hex[index * 2]);
        const int low = hex_digit(hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            result.error = "ADS-B frame contains invalid hexadecimal digits";
            return result;
        }
        bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    if (bytes[0] >> 3 != 17) {
        result.error = "Only ADS-B DF17 frames are supported";
        return result;
    }
    if (syndrome(bytes) != 0) {
        result.error = "ADS-B parity check failed";
        return result;
    }
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t index = 1; index < 4; ++index) {
        result.aircraft_address += digits[bytes[index] >> 4];
        result.aircraft_address += digits[bytes[index] & 15];
    }
    result.type_code = bytes[4] >> 3;
    if (result.type_code >= 1 && result.type_code <= 4) {
        result.emitter_category = bytes[4] & 7U;
        uint64_t bits = 0;
        for (std::size_t index = 5; index <= 10; ++index) bits = (bits << 8) | bytes[index];
        for (int shift = 42; shift >= 0; shift -= 6) {
            const auto code = static_cast<unsigned>((bits >> shift) & 63U);
            if (code >= 1 && code <= 26) result.callsign += static_cast<char>('A' + code - 1);
            else if (code >= 48 && code <= 57) result.callsign += static_cast<char>('0' + code - 48);
            else if (code == 0 || code == 32) result.callsign += ' ';
            else result.callsign += '?';
        }
        const auto first = result.callsign.find_first_not_of(' ');
        if (first == std::string::npos) result.callsign.clear();
        else result.callsign = result.callsign.substr(first, result.callsign.find_last_not_of(' ') - first + 1);
    } else if (result.type_code >= 9 && result.type_code <= 18) {
        result.airborne_cpr_available = true;
        result.cpr_odd = (bytes[6] & 0x04U) != 0;
        result.cpr_latitude = (static_cast<uint32_t>(bytes[6] & 0x03U) << 15) |
                              (static_cast<uint32_t>(bytes[7]) << 7) | (bytes[8] >> 1);
        result.cpr_longitude = (static_cast<uint32_t>(bytes[8] & 0x01U) << 16) |
                               (static_cast<uint32_t>(bytes[9]) << 8) | bytes[10];
        const unsigned altitude = (static_cast<unsigned>(bytes[5]) << 4) | (bytes[6] >> 4);
        if (altitude != 0 && (altitude & 0x10U)) {
            const unsigned steps = ((altitude & 0xfe0U) >> 1) | (altitude & 0xfU);
            result.altitude_available = true;
            result.altitude_feet = static_cast<int>(steps * 25) - 1000;
        }
    } else if (result.type_code == 19) {
        uint64_t me = 0;
        for (std::size_t index = 4; index <= 10; ++index) me = (me << 8) | bytes[index];
        const unsigned subtype = bytes[4] & 7U;
        if (subtype >= 1 && subtype <= 4) {
            const unsigned first = static_cast<unsigned>((me >> 32) & 0x3ffU);
            const unsigned second = static_cast<unsigned>((me >> 21) & 0x3ffU);
            const int multiplier = (subtype == 2 || subtype == 4) ? 4 : 1;
            if (subtype <= 2) {
                result.speed_type = AdsbSpeedType::Ground;
                if (first && second) {
                    const int east = static_cast<int>(first - 1) * multiplier * ((me >> 42) & 1U ? -1 : 1);
                    const int north = static_cast<int>(second - 1) * multiplier * ((me >> 31) & 1U ? -1 : 1);
                    result.speed_knots = std::hypot(static_cast<double>(east), static_cast<double>(north));
                    result.speed_available = true;
                    if (east || north) {
                        constexpr double degrees = 180.0 / 3.14159265358979323846;
                        result.direction_degrees = std::atan2(static_cast<double>(east),
                                                              static_cast<double>(north)) * degrees;
                        if (result.direction_degrees < 0) result.direction_degrees += 360.0;
                        result.direction_available = true;
                    }
                }
            } else {
                result.speed_type = (me >> 31) & 1U ? AdsbSpeedType::TrueAirspeed
                                                    : AdsbSpeedType::IndicatedAirspeed;
                if (second) {
                    result.speed_knots = (second - 1) * multiplier;
                    result.speed_available = true;
                }
                if ((me >> 42) & 1U) {
                    result.direction_degrees = first * (360.0 / 1024.0);
                    result.direction_available = true;
                }
            }
            const unsigned rate = static_cast<unsigned>((me >> 10) & 0x1ffU);
            if (rate) {
                result.vertical_rate_available = true;
                result.vertical_rate_barometric = ((me >> 20) & 1U) != 0;
                result.vertical_rate_fpm = static_cast<int>((rate - 1) * 64) *
                                           (((me >> 19) & 1U) ? -1 : 1);
            }
        }
    } else if (result.type_code == 28 && (bytes[4] & 7U) == 1) {
        result.emergency_status_available = true;
        result.emergency_status = bytes[5] >> 5;
        const unsigned identity = ((static_cast<unsigned>(bytes[5]) & 0x1fU) << 8) | bytes[6];
        if (!(identity & 0x40U)) {
            const auto bit = [identity](unsigned position) { return (identity >> (12 - position)) & 1U; };
            const auto digit = [&](unsigned one, unsigned two, unsigned four) {
                return static_cast<char>('0' + bit(one) + 2 * bit(two) + 4 * bit(four));
            };
            result.squawk += digit(1, 3, 5);
            result.squawk += digit(7, 9, 11);
            result.squawk += digit(0, 2, 4);
            result.squawk += digit(8, 10, 12);
        }
    }
    return result;
}

AdsbFrame AdsbPositionDecoder::update(AdsbFrame frame,
                                       std::chrono::steady_clock::time_point received) {
    frame.position_available = false;
    if (!frame.error.empty() || !frame.airborne_cpr_available || frame.aircraft_address.empty() ||
        frame.cpr_latitude >= 131072 || frame.cpr_longitude >= 131072)
        return frame;
    auto it = std::find_if(aircraft_.begin(), aircraft_.end(), [&](const Aircraft& item) {
        return item.address == frame.aircraft_address;
    });
    if (it == aircraft_.end()) {
        if (aircraft_.size() == 128) {
            it = std::min_element(aircraft_.begin(), aircraft_.end(),
                                  [](const Aircraft& a, const Aircraft& b) {
                                      return a.last_received < b.last_received;
                                  });
            *it = Aircraft{};
        } else {
            aircraft_.emplace_back();
            it = aircraft_.end() - 1;
        }
        it->address = frame.aircraft_address;
    } else if (received < it->last_received) {
        it->even.valid = false;
        it->odd.valid = false;
    }
    it->last_received = received;
    auto& current = frame.cpr_odd ? it->odd : it->even;
    current = {true, frame.cpr_latitude, frame.cpr_longitude, received};
    const auto& even = it->even;
    const auto& odd = it->odd;
    if (!even.valid || !odd.valid || received - (frame.cpr_odd ? even.received : odd.received) >
                                           std::chrono::seconds(10))
        return frame;

    constexpr double scale = 131072.0;
    const double even_lat = even.latitude / scale, odd_lat = odd.latitude / scale;
    const int j = static_cast<int>(std::floor(59.0 * even_lat - 60.0 * odd_lat + 0.5));
    double latitude_even = 6.0 * (cpr_mod(j, 60) + even_lat);
    double latitude_odd = (360.0 / 59.0) * (cpr_mod(j, 59) + odd_lat);
    if (latitude_even >= 270.0) latitude_even -= 360.0;
    if (latitude_odd >= 270.0) latitude_odd -= 360.0;
    if (std::abs(latitude_even) > 90.0 || std::abs(latitude_odd) > 90.0 ||
        longitude_zones(latitude_even) != longitude_zones(latitude_odd))
        return frame;
    const double latitude = frame.cpr_odd ? latitude_odd : latitude_even;
    const int zones = longitude_zones(latitude);
    const int n = std::max(zones - (frame.cpr_odd ? 1 : 0), 1);
    const double even_lon = even.longitude / scale, odd_lon = odd.longitude / scale;
    const int m = static_cast<int>(std::floor(even_lon * (zones - 1) - odd_lon * zones + 0.5));
    double longitude = (360.0 / n) * (cpr_mod(m, n) + (frame.cpr_odd ? odd_lon : even_lon));
    if (longitude > 180.0) longitude -= 360.0;
    frame.latitude = latitude;
    frame.longitude = longitude;
    frame.position_available = true;
    return frame;
}

} // namespace platform
