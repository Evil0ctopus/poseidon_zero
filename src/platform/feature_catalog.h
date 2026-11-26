#pragma once

#include <array>
#include <cstdint>

namespace platform {

enum class FeatureRisk {
    Safe,
    Authorized,
    Experimental,
};

struct FeatureDefinition {
    const char* name;
    const char* description;
    FeatureRisk risk;
    bool available;
};

using FeatureList = std::array<FeatureDefinition, 8>;
using NetworkFeatureList = std::array<FeatureDefinition, 12>;
using ToolFeatureList = std::array<FeatureDefinition, 22>;

NetworkFeatureList network_features();
FeatureList wireless_features();
FeatureList infrared_features();
FeatureList expansion_features();
ToolFeatureList tools_features();
FeatureList media_features();

} // namespace platform
