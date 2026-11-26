#include "survey_qr.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool write_pgm(const std::filesystem::path& path, const app::SurveyQr& qr) {
    std::ofstream out(path, std::ios::binary);
    out << "P5\n" << qr.width << ' ' << qr.width << "\n255\n";
    for (std::size_t i = 0; i < qr.pixels.size(); i += 4)
        out.put(static_cast<char>(qr.pixels[i]));
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv) {
    using platform::SurveyTimelineEntry;
    const SurveyTimelineEntry normal{"private-time", "private-source", "private-name",
                                     "38.627", "-90.1994", "private-indicator"};
    const SurveyTimelineEntry extreme{"", "", "", "-90", "-180", ""};
    const SurveyTimelineEntry zero{"", "", "", "0", "0", ""};
    bool ok = true;
    const auto geo = platform::survey_geo_uri(normal);
    ok &= check(geo.error.empty() && geo.uri == "geo:38.6270000,-90.1994000",
                "QR payload must contain only receiver coordinates");
    ok &= check(platform::survey_geo_uri(extreme).uri == "geo:-90.0000000,-180.0000000",
                "Geographic limits should be supported");
    ok &= check(!platform::survey_geo_uri({"", "", "", "", "", ""}).error.empty() &&
                !platform::survey_geo_uri({"", "", "", "0", "", ""}).error.empty() &&
                !platform::survey_geo_uri({"", "", "", "91", "0", ""}).error.empty() &&
                !platform::survey_geo_uri({"", "", "", "0", "180.1", ""}).error.empty() &&
                !platform::survey_geo_uri({"", "", "", "nan", "0", ""}).error.empty(),
                "Missing or invalid GPS must not produce a QR");
    const auto a = app::make_survey_qr(normal);
    const auto b = app::make_survey_qr(extreme);
    const auto c = app::make_survey_qr(zero);
    for (const auto* qr : {&a, &b, &c}) {
        ok &= check(qr->error.empty() && qr->width >= 116 && qr->width <= 148 &&
                    qr->pixels.size() == static_cast<std::size_t>(qr->width) * qr->width * 4 &&
                    qr->pixels.front() == 255, "QR dimensions, buffer, or quiet zone invalid");
    }
    ok &= check(!app::make_survey_qr({"", "", "", "", "", ""}).error.empty(),
                "No GPS must fail QR generation");
    if (argc == 2 && ok) {
        const std::filesystem::path directory(argv[1]);
        ok &= check(write_pgm(directory / "normal.pgm", a) &&
                    write_pgm(directory / "extreme.pgm", b) &&
                    write_pgm(directory / "zero.pgm", c), "Cannot write QR verification images");
    }
    return ok ? 0 : 1;
}
