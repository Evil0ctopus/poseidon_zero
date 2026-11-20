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
using ToolFeatureList = std::array<FeatureDefinition, 17>;

FeatureList network_features();
FeatureList wireless_features();
FeatureList infrared_features();
FeatureList expansion_features();
ToolFeatureList tools_features();
FeatureList media_features();

} // namespace platform
