#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace platform {

struct RemoteIdPosition {
    double latitude;
    double longitude;
    std::chrono::system_clock::time_point observed_at;
};

struct RemoteIdData {
    std::string aircraft_id;
    std::string aircraft_id_type;
    std::chrono::system_clock::time_point id_observed_at{};
    std::optional<RemoteIdPosition> position;
};

bool update_remote_id(RemoteIdData& data, const std::uint8_t* bytes, std::size_t length,
                      std::chrono::system_clock::time_point observed_at);

} // namespace platform
