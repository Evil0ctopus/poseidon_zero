#include "remote_id.h"

#include <utility>

namespace platform {
namespace {

constexpr std::size_t kMessageSize = 25;

std::int64_t signed_le32(const std::uint8_t* bytes) {
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    return value < 0x80000000U ? static_cast<std::int64_t>(value)
                               : static_cast<std::int64_t>(value) - 0x100000000LL;
}

void apply_message(RemoteIdData& data, const std::uint8_t* message,
                   std::chrono::system_clock::time_point observed_at) {
    if ((message[0] & 0x0f) > 2) return;
    const auto type = message[0] >> 4;
    if (type == 0) {
        const auto id_type = message[1] >> 4;
        if (id_type != 1 && id_type != 2) return;
        std::string id;
        bool padding = false;
        for (std::size_t i = 2; i < 22; ++i) {
            const auto c = message[i];
            if (c == 0) {
                padding = true;
            } else if (padding || c < 32 || c > 126) {
                return;
            } else {
                id += static_cast<char>(c);
            }
        }
        if (id.empty()) return;
        data.aircraft_id = std::move(id);
        data.aircraft_id_type = id_type == 1 ? "serial" : "caa_registration";
        data.id_observed_at = observed_at;
    } else if (type == 1) {
        const auto latitude = signed_le32(message + 5);
        const auto longitude = signed_le32(message + 9);
        if ((latitude == 0 && longitude == 0) ||
            latitude < -900000000LL || latitude > 900000000LL ||
            longitude < -1800000000LL || longitude > 1800000000LL) {
            data.position.reset();
            return;
        }
        data.position = RemoteIdPosition{latitude / 10000000.0, longitude / 10000000.0,
                                         observed_at};
    }
}

} // namespace

bool update_remote_id(RemoteIdData& data, const std::uint8_t* bytes, std::size_t length,
                      std::chrono::system_clock::time_point observed_at) {
    if (!bytes || length < 27 || bytes[0] != 0x0d) return false;
    const auto* message = bytes + 2;
    const auto size = length - 2;
    if (size == kMessageSize) {
        apply_message(data, message, observed_at);
        return true;
    }
    if (size < 3 || (message[0] >> 4) != 0x0f || (message[0] & 0x0f) > 2 ||
        message[1] != kMessageSize || message[2] == 0 || message[2] > 9 ||
        size != 3 + static_cast<std::size_t>(message[2]) * kMessageSize) return false;
    for (std::size_t i = 0; i < message[2]; ++i)
        apply_message(data, message + 3 + i * kMessageSize, observed_at);
    return true;
}

} // namespace platform
