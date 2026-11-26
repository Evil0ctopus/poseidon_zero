#pragma once

#include "gnss.h"
#include "remote_id.h"
#include "unique_file.h"

#include <chrono>
#include <atomic>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platform {

struct SurveyObservation {
    std::chrono::system_clock::time_point recorded_at;
    std::string source;
    std::string name;
    std::string address;
    std::string signal;
    std::string signal_unit;
    std::string frequency_mhz;
    std::string channel;
    std::string security;
    std::string freshness;
    bool scan_may_be_limited{false};
    std::optional<GnssFix> location;
    std::string indicators;
    RemoteIdData remote_id;
};

struct SurveyFiles {
    std::vector<std::string> names;
    std::string error;
};

struct SurveyTimelineEntry {
    std::string recorded_utc;
    std::string source;
    std::string name;
    std::string gps_latitude;
    std::string gps_longitude;
    std::string indicators;
};

struct SurveyTimeline {
    std::vector<SurveyTimelineEntry> entries;
    std::size_t total_rows{0};
    bool indicators_available{false};
    std::string error;
};

constexpr std::size_t kSurveyPlotWidth = 20;
constexpr std::size_t kSurveyPlotHeight = 8;
struct SurveyPlot {
    std::array<bool, kSurveyPlotWidth * kSurveyPlotHeight> cells{};
    std::size_t gps_rows{0};
    std::string error;
};

bool survey_watchlist_match(const std::string& indicators);
SurveyPlot make_survey_plot(const SurveyTimeline& timeline);

struct SurveyMapTile {
    uint32_t zoom{0};
    uint32_t x{0};
    uint32_t y{0};
    uint32_t pixel_x{0};
    uint32_t pixel_y{0};
    std::string error;
};
SurveyMapTile survey_map_tile(const SurveyTimelineEntry& entry, uint32_t zoom);

struct SurveyGeoUri {
    std::string uri;
    std::string error;
};
SurveyGeoUri survey_geo_uri(const SurveyTimelineEntry& entry);

struct SurveyMapAttribution {
    std::string text;
    std::vector<uint32_t> glyphs;
    std::string error;
};
SurveyMapAttribution read_survey_map_attribution(const std::filesystem::path& path);

SurveyTimeline read_survey_timeline(const std::filesystem::path& directory,
                                    const std::string& name, const std::atomic_bool& cancelled);

struct SurveyGpxExport {
    std::filesystem::path path;
    std::size_t points{0};
    std::string error;
};

SurveyGpxExport export_survey_gpx(const std::filesystem::path& directory,
                                  const SurveyTimeline& timeline);

struct FieldNote {
    std::string filename;
    std::string recorded_utc;
    std::string text;
    std::string error;
};

FieldNote save_field_note(const std::filesystem::path& directory, const std::string& text);
FieldNote read_field_note(const std::filesystem::path& directory, const std::string& filename,
                          const std::atomic_bool& cancelled);

class SurveyFile {
public:
    static std::unique_ptr<SurveyFile> create(const std::filesystem::path& directory,
                                               std::string& error);
    ~SurveyFile();
    SurveyFile(const SurveyFile&) = delete;
    SurveyFile& operator=(const SurveyFile&) = delete;

    bool append(const SurveyObservation& observation, std::string& error);
    bool close(std::string& error);
    const std::filesystem::path& path() const;

private:
    explicit SurveyFile(UniqueFile file);
    UniqueFile file_;
    std::size_t bytes_{0};
};

SurveyFiles list_survey_files(const std::filesystem::path& directory);
std::string delete_survey_file(const std::filesystem::path& directory, const std::string& name);
std::string delete_all_survey_files(const std::filesystem::path& directory);

} // namespace platform
