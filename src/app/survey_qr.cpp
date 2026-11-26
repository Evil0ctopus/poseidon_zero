#include "survey_qr.h"

#include "src/libs/qrcode/qrcodegen.h"

#include <array>

namespace app {

SurveyQr make_survey_qr(const platform::SurveyTimelineEntry& entry) {
    const auto geo = platform::survey_geo_uri(entry);
    if (!geo.error.empty()) return {0, {}, geo.error};
    constexpr int max_version = 3;
    constexpr int scale = 4;
    constexpr int quiet_zone = 4;
    std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(max_version)> scratch{};
    std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(max_version)> code{};
    if (!qrcodegen_encodeText(geo.uri.c_str(), scratch.data(), code.data(),
                              qrcodegen_Ecc_MEDIUM, 1, max_version,
                              qrcodegen_Mask_AUTO, true))
        return {0, {}, "Could not encode receiver GPS"};
    const int modules = qrcodegen_getSize(code.data());
    const auto width = static_cast<uint32_t>((modules + 2 * quiet_zone) * scale);
    SurveyQr result;
    result.width = width;
    result.pixels.assign(static_cast<std::size_t>(width) * width * 4, 0xff);
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qrcodegen_getModule(code.data(), x, y)) continue;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    const auto px = static_cast<uint32_t>((x + quiet_zone) * scale + dx);
                    const auto py = static_cast<uint32_t>((y + quiet_zone) * scale + dy);
                    const auto offset = (static_cast<std::size_t>(py) * width + px) * 4;
                    result.pixels[offset] = result.pixels[offset + 1] = result.pixels[offset + 2] = 0;
                }
            }
        }
    }
    return result;
}

} // namespace app
