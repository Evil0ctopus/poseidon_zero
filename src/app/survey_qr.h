#pragma once

#include "survey_file.h"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

struct SurveyQr {
    uint32_t width{0};
    std::vector<uint8_t> pixels;
    std::string error;
};

SurveyQr make_survey_qr(const platform::SurveyTimelineEntry& entry);

} // namespace app
