#include "survey_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iomanip>
#include <locale>
#include <sstream>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace platform {
namespace {

constexpr std::size_t kMaxSessionBytes = 10 * 1024 * 1024;
constexpr std::size_t kMaxSessions = 256;
constexpr const char* kHeader =
    "version,recorded_utc,source,name,address,signal,signal_unit,frequency_mhz,channel,"
    "security,freshness,scan_may_be_limited,gps_latitude,gps_longitude,gps_fix_utc,gps_fix_mode,"
    "unverified_indicators,drone_id,drone_id_type,drone_id_seen_utc,"
    "drone_latitude,drone_longitude,drone_position_seen_utc\n";

std::string io_error(const char* action) {
    return std::string(action) + ": " + (errno ? std::strerror(errno) : "I/O error");
}

std::string directory_error(const std::filesystem::path& directory) {
    const auto error = ensure_confidential_directory(directory);
    return error.empty() ? "" : "Survey directory: " + error;
}

bool valid_filename(const std::string& name) {
    return is_unique_filename(name, "survey-", ".csv");
}

std::string utc(std::chrono::system_clock::time_point point) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(point);
    const auto fraction = std::chrono::duration_cast<std::chrono::milliseconds>(point - seconds);
    const std::time_t value = std::chrono::system_clock::to_time_t(seconds);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &value) != 0) return {};
#else
    if (!gmtime_r(&value, &tm)) return {};
#endif
    char text[32]{};
    if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &tm) == 0) return {};
    char result[48]{};
    std::snprintf(result, sizeof(result), "%s.%03lldZ", text,
                  static_cast<long long>(fraction.count()));
    return result;
}

std::string csv(std::string value) {
    // Quoting alone does not stop spreadsheet software interpreting formulas.
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && std::string("=+-@").find(value[first]) != std::string::npos)
        value.insert(value.begin(), '\'');
    std::string result{"\""};
    for (char c : value) {
        if (c == '"') result += '"';
        result += c;
    }
    return result + '"';
}

std::string coordinate(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(7) << value;
    return out.str();
}

bool valid_coordinate(const std::string& value, double limit, double* parsed = nullptr) {
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    double number = 0;
    input >> std::noskipws >> number;
    const bool valid = input && input.peek() == std::char_traits<char>::eof() &&
                       std::isfinite(number) && number >= -limit && number <= limit;
    if (valid && parsed) *parsed = number;
    return valid;
}

} // namespace

bool survey_watchlist_match(const std::string& indicators) {
    constexpr char prefix[] = "user-name:";
    std::size_t start = 0;
    while (start < indicators.size()) {
        const auto end = indicators.find(';', start);
        const auto length = (end == std::string::npos ? indicators.size() : end) - start;
        if (length > sizeof(prefix) - 1 &&
            indicators.compare(start, sizeof(prefix) - 1, prefix) == 0)
            return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

SurveyGeoUri survey_geo_uri(const SurveyTimelineEntry& entry) {
    if (entry.gps_latitude.empty() && entry.gps_longitude.empty())
        return {{}, "No receiver GPS in selected row"};
    double latitude = 0, longitude = 0;
    if (!valid_coordinate(entry.gps_latitude, 90, &latitude) ||
        !valid_coordinate(entry.gps_longitude, 180, &longitude))
        return {{}, "Invalid receiver GPS coordinates"};
    return {"geo:" + coordinate(latitude) + "," + coordinate(longitude), {}};
}

SurveyMapTile survey_map_tile(const SurveyTimelineEntry& entry, uint32_t zoom) {
    SurveyMapTile tile;
    tile.zoom = zoom;
    if (zoom > 18) {
        tile.error = "Map zoom must be 0 to 18";
        return tile;
    }
    if (entry.gps_latitude.empty() && entry.gps_longitude.empty()) {
        tile.error = "No receiver GPS in selected row";
        return tile;
    }
    double latitude = 0, longitude = 0;
    if (!valid_coordinate(entry.gps_latitude, 90, &latitude) ||
        !valid_coordinate(entry.gps_longitude, 180, &longitude)) {
        tile.error = "Invalid receiver GPS coordinates";
        return tile;
    }
    constexpr double max_latitude = 85.0511287798066;
    if (std::abs(latitude) > max_latitude) {
        tile.error = "GPS outside Web Mercator map";
        return tile;
    }
    constexpr double pi = 3.14159265358979323846;
    const double count = static_cast<double>(uint32_t{1} << zoom);
    if (longitude == 180.0) longitude = -180.0;
    const double x = std::clamp((longitude + 180.0) / 360.0 * count,
                                0.0, std::nextafter(count, 0.0));
    const double y = std::clamp((1.0 - std::asinh(std::tan(latitude * pi / 180.0)) / pi)
                                * 0.5 * count, 0.0, std::nextafter(count, 0.0));
    tile.x = static_cast<uint32_t>(x);
    tile.y = static_cast<uint32_t>(y);
    tile.pixel_x = static_cast<uint32_t>((x - tile.x) * 256);
    tile.pixel_y = static_cast<uint32_t>((y - tile.y) * 256);
    return tile;
}

SurveyMapAttribution read_survey_map_attribution(const std::filesystem::path& path) {
    SurveyMapAttribution result;
    std::array<char, 82> buffer{};
    std::size_t count = 0;
#if defined(_WIN32)
    HANDLE input = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        result.error = "Cannot open map attribution";
        return result;
    }
    struct InputGuard {
        HANDLE handle;
        ~InputGuard() { CloseHandle(handle); }
    } guard{input};
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(input) != FILE_TYPE_DISK || !GetFileInformationByHandle(input, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nFileSizeHigh || info.nFileSizeLow > 82) {
        result.error = "Map attribution must be a small regular file";
        return result;
    }
    DWORD read_bytes = 0;
    if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read_bytes, nullptr)) {
        result.error = "Cannot read map attribution";
        return result;
    }
    count = read_bytes;
    if (count != info.nFileSizeLow) {
        result.error = "Map attribution changed while reading";
        return result;
    }
#else
    const int input = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) {
        result.error = std::string("Cannot open map attribution: ") + std::strerror(errno);
        return result;
    }
    struct InputGuard {
        int fd;
        ~InputGuard() { close(fd); }
    } guard{input};
    struct stat info{};
    if (fstat(input, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || info.st_size > 82) {
        result.error = "Map attribution must be a small regular file";
        return result;
    }
    while (count < buffer.size()) {
        const auto received = read(input, buffer.data() + count, buffer.size() - count);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0) {
            result.error = std::string("Cannot read map attribution: ") + std::strerror(errno);
            return result;
        }
        if (received == 0) break;
        count += static_cast<std::size_t>(received);
    }
    if (count != static_cast<std::size_t>(info.st_size)) {
        result.error = "Map attribution changed while reading";
        return result;
    }
#endif
    if (count && buffer[count - 1] == '\n') --count;
    if (count && buffer[count - 1] == '\r') --count;
    if (count == 0 || count > 80) {
        result.error = "Map attribution must contain 1 to 80 bytes";
        return result;
    }
    for (std::size_t i = 0; i < count;) {
        const auto first = static_cast<unsigned char>(buffer[i++]);
        uint32_t codepoint = 0;
        unsigned remaining = 0;
        if (first >= 0x20 && first <= 0x7e) {
            result.glyphs.push_back(first);
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) { codepoint = first & 0x1f; remaining = 1; }
        else if (first >= 0xe0 && first <= 0xef) { codepoint = first & 0x0f; remaining = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { codepoint = first & 0x07; remaining = 3; }
        else { result.error = "Invalid map attribution text"; return result; }
        const unsigned length = remaining;
        if (i + remaining > count) { result.error = "Invalid map attribution text"; return result; }
        for (unsigned j = 0; j < remaining; ++j) {
            const auto next = static_cast<unsigned char>(buffer[i++]);
            if ((next & 0xc0) != 0x80) {
                result.error = "Invalid map attribution text";
                return result;
            }
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((length == 1 && codepoint < 0xa0) ||
            (length == 2 && codepoint < 0x800) ||
            (length == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
            result.error = "Invalid map attribution text";
            return result;
        }
        result.glyphs.push_back(codepoint);
    }
    result.text.assign(buffer.data(), count);
    return result;
}

SurveyPlot make_survey_plot(const SurveyTimeline& timeline) {
    SurveyPlot plot;
    if (!timeline.error.empty()) { plot.error = timeline.error; return plot; }
    struct Point { double latitude; double longitude; };
    std::vector<Point> points;
    std::vector<double> longitudes;
    for (const auto& entry : timeline.entries) {
        if (entry.gps_latitude.empty() && entry.gps_longitude.empty()) continue;
        double latitude = 0, longitude = 0;
        if (!valid_coordinate(entry.gps_latitude, 90, &latitude) ||
            !valid_coordinate(entry.gps_longitude, 180, &longitude)) {
            plot.error = "Invalid receiver GPS coordinates";
            return plot;
        }
        longitude = std::fmod(longitude + 360.0, 360.0);
        points.push_back({latitude, longitude});
        longitudes.push_back(longitude);
    }
    plot.gps_rows = points.size();
    if (points.empty()) return plot;
    std::sort(longitudes.begin(), longitudes.end());
    double largest_gap = -1;
    std::size_t origin_index = 0;
    for (std::size_t i = 0; i < longitudes.size(); ++i) {
        const double next = i + 1 < longitudes.size() ? longitudes[i + 1] : longitudes[0] + 360.0;
        const double gap = next - longitudes[i];
        if (gap > largest_gap) {
            largest_gap = gap;
            origin_index = (i + 1) % longitudes.size();
        }
    }
    const double origin = longitudes[origin_index];
    const double lon_span = 360.0 - largest_gap;
    const auto latitude_range = std::minmax_element(
        points.begin(), points.end(), [](const Point& a, const Point& b) { return a.latitude < b.latitude; });
    const double south = latitude_range.first->latitude;
    const double lat_span = latitude_range.second->latitude - south;
    for (const auto& point : points) {
        const double east = std::fmod(point.longitude - origin + 360.0, 360.0);
        const auto x = lon_span > 0
            ? static_cast<std::size_t>(std::lround((east / lon_span) * (kSurveyPlotWidth - 1)))
            : (kSurveyPlotWidth - 1) / 2;
        const auto y = lat_span > 0
            ? (kSurveyPlotHeight - 1) -
                static_cast<std::size_t>(std::lround(((point.latitude - south) / lat_span) *
                                                     (kSurveyPlotHeight - 1)))
            : (kSurveyPlotHeight - 1) / 2;
        if (x >= kSurveyPlotWidth || y >= kSurveyPlotHeight) {
            plot.error = "Cannot scale receiver GPS coordinates";
            return plot;
        }
        plot.cells[y * kSurveyPlotWidth + x] = true;
    }
    return plot;
}

static SurveyTimeline read_survey_timeline_bounded(const std::filesystem::path& directory,
                                                    const std::string& name,
                                                    const std::atomic_bool& cancelled,
                                                    std::size_t max_bytes) {
    SurveyTimeline result;
    if (!valid_filename(name)) { result.error = "Invalid survey filename"; return result; }
    if (cancelled) { result.error = "Timeline cancelled"; return result; }
    result.error = directory_error(directory);
    if (!result.error.empty()) return result;
    const auto path = directory / std::filesystem::u8path(name);
#if defined(_WIN32)
    HANDLE input = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (input == INVALID_HANDLE_VALUE) { result.error = "Cannot open survey"; return result; }
    struct InputGuard {
        HANDLE handle;
        ~InputGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } guard{input};
    BY_HANDLE_FILE_INFORMATION initial{};
    if (GetFileType(input) != FILE_TYPE_DISK || !GetFileInformationByHandle(input, &initial) ||
        (initial.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        initial.nFileSizeHigh || initial.nFileSizeLow > max_bytes) {
        result.error = "File must be regular and within the size limit"; return result;
    }
#else
    const int input = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) { result.error = std::string("Cannot open survey: ") + std::strerror(errno); return result; }
    struct InputGuard {
        int fd;
        ~InputGuard() { if (fd >= 0) close(fd); }
    } guard{input};
    struct stat initial{};
    if (fstat(input, &initial) != 0 || !S_ISREG(initial.st_mode) || initial.st_size < 0 ||
        static_cast<std::size_t>(initial.st_size) > max_bytes) {
        result.error = "File must be regular and within the size limit"; return result;
    }
#endif
    std::array<char, 8192> buffer{};
    std::string data;
    for (;;) {
        if (cancelled) { result.error = "Timeline cancelled"; break; }
#if defined(_WIN32)
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            result.error = "Cannot read survey"; break;
        }
#else
        const auto count = read(input, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result.error = std::string("Cannot read survey: ") + std::strerror(errno); break; }
#endif
        if (!count) break;
        if (data.size() + static_cast<std::size_t>(count) > max_bytes) {
            result.error = "File exceeds the size limit"; break;
        }
        data.append(buffer.data(), static_cast<std::size_t>(count));
    }
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION final_info{};
    if (result.error.empty() &&
        (!GetFileInformationByHandle(input, &final_info) ||
         final_info.nFileSizeHigh != initial.nFileSizeHigh ||
         final_info.nFileSizeLow != initial.nFileSizeLow ||
         CompareFileTime(&final_info.ftLastWriteTime, &initial.ftLastWriteTime) != 0 ||
         data.size() != initial.nFileSizeLow))
        result.error = "Survey changed while reading";
    if (!CloseHandle(input)) {
        if (!result.error.empty()) result.error += "; ";
        result.error += "Cannot close survey";
    }
    guard.handle = INVALID_HANDLE_VALUE;
#else
    struct stat final_status{};
    if (result.error.empty() &&
        (fstat(input, &final_status) != 0 || final_status.st_size != initial.st_size ||
         final_status.st_mtim.tv_sec != initial.st_mtim.tv_sec ||
         final_status.st_mtim.tv_nsec != initial.st_mtim.tv_nsec ||
         final_status.st_ctim.tv_sec != initial.st_ctim.tv_sec ||
         final_status.st_ctim.tv_nsec != initial.st_ctim.tv_nsec ||
         data.size() != static_cast<std::size_t>(initial.st_size)))
        result.error = "Survey changed while reading";
    if (close(input) != 0) {
        if (!result.error.empty()) result.error += "; ";
        result.error += std::string("Cannot close survey: ") + std::strerror(errno);
    }
    guard.fd = -1;
#endif
    if (!result.error.empty()) return result;
    const auto header_end = data.find('\n');
    if (header_end == std::string::npos || header_end > 4096) {
        result.error = "Unsupported survey format";
        return result;
    }
    std::vector<std::string> columns;
    std::size_t column_start = 0;
    for (std::size_t i = 0; i <= header_end; ++i) {
        if (i == header_end || data[i] == ',') {
            const auto column = data.substr(column_start, i - column_start);
            if (column.empty() || columns.size() == 64 ||
                !std::all_of(column.begin(), column.end(), [](char ch) {
                    return (ch >= 'a' && ch <= 'z') || ch == '_';
                }) || std::find(columns.begin(), columns.end(), column) != columns.end()) {
                result.error = "Unsupported survey header";
                return result;
            }
            columns.push_back(column);
            column_start = i + 1;
        }
    }
    const auto column_index = [&](const char* name) {
        return static_cast<std::size_t>(std::find(columns.begin(), columns.end(), name) - columns.begin());
    };
    const auto version = column_index("version"), timestamp = column_index("recorded_utc");
    const auto source = column_index("source"), name_index = column_index("name");
    const auto latitude = column_index("gps_latitude"), longitude = column_index("gps_longitude");
    const auto indicators = column_index("unverified_indicators");
    result.indicators_available = indicators != columns.size();
    if (version == columns.size() || timestamp == columns.size() || source == columns.size() ||
        name_index == columns.size() || latitude == columns.size() || longitude == columns.size()) {
        result.error = "Unsupported survey header";
        return result;
    }
    std::deque<SurveyTimelineEntry> recent;
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false, ended_quote = false;
    std::size_t until_cancel_check = 0;
    for (std::size_t index = header_end + 1; index < data.size(); ++index) {
        if (until_cancel_check == 0) {
            if (cancelled) { result.error = "Timeline cancelled"; return result; }
            until_cancel_check = 8192;
        }
        --until_cancel_check;
        const char ch = data[index];
        if (quoted) {
            if (ch == '"') {
                if (index + 1 < data.size() && data[index + 1] == '"') { field += '"'; ++index; }
                else { quoted = false; ended_quote = true; }
            } else if (ch == '\0') { result.error = "Invalid survey CSV"; return result; }
            else field += ch;
        } else if (ch == ',' || ch == '\n') {
            fields.push_back(std::move(field));
            field.clear();
            ended_quote = false;
            if (fields.size() > columns.size()) { result.error = "Invalid survey row"; return result; }
            if (ch == '\n') {
                if (fields.size() != columns.size() ||
                    (fields[version] != "1" && fields[version] != "2" && fields[version] != "3") ||
                    (fields[latitude].empty() != fields[longitude].empty()) ||
                    (!fields[latitude].empty() && (!valid_coordinate(fields[latitude], 90) ||
                                                   !valid_coordinate(fields[longitude], 180)))) {
                    result.error = "Invalid survey row";
                    return result;
                }
                recent.push_back({fields[timestamp], fields[source], fields[name_index],
                                  fields[latitude], fields[longitude],
                                  result.indicators_available ? fields[indicators] : ""});
                if (recent.size() > 128) recent.pop_front();
                ++result.total_rows;
                fields.clear();
            }
        } else if (ch == '"' && !ended_quote && field.empty()) {
            quoted = true;
        } else if (ended_quote || ch == '"' || ch == '\r' || ch == '\0') {
            result.error = "Invalid survey CSV";
            return result;
        } else field += ch;
        if (field.size() > 4096) { result.error = "Survey field too large"; return result; }
    }
    if (quoted || ended_quote || !fields.empty() || !field.empty()) {
        result.error = "Incomplete survey row";
        return result;
    }
    result.entries.assign(recent.begin(), recent.end());
    return result;
}

SurveyTimeline read_survey_timeline(const std::filesystem::path& directory,
                                    const std::string& name, const std::atomic_bool& cancelled) {
    return read_survey_timeline_bounded(directory, name, cancelled, kMaxSessionBytes);
}

SurveyGpxExport export_survey_gpx(const std::filesystem::path& directory,
                                  const SurveyTimeline& timeline) {
    SurveyGpxExport result;
    if (!timeline.error.empty()) { result.error = timeline.error; return result; }
    if (timeline.entries.size() > 128) {
        result.error = "Too many timeline rows";
        return result;
    }
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<gpx version=\"1.1\" creator=\"POSEIDON\" "
                      "xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
    for (const auto& entry : timeline.entries) {
        if (entry.gps_latitude.empty() && entry.gps_longitude.empty()) continue;
        double latitude = 0, longitude = 0;
        if (!valid_coordinate(entry.gps_latitude, 90, &latitude) ||
            !valid_coordinate(entry.gps_longitude, 180, &longitude)) {
            result.error = "Invalid receiver GPS coordinates";
            return result;
        }
        xml += "  <wpt lat=\"" + coordinate(latitude) + "\" lon=\"" +
               coordinate(longitude) + "\"/>\n";
        ++result.points;
    }
    if (!result.points) {
        result.error = "No receiver GPS points in retained rows";
        return result;
    }
    xml += "</gpx>\n";
    result.error = directory_error(directory);
    if (!result.error.empty()) return result;
    auto file = open_unique_file(directory, "survey-gps-", ".gpx");
    if (!file.error.empty()) { result.error = file.error; return result; }
    errno = 0;
    if (std::fwrite(xml.data(), 1, xml.size(), file.stream.get()) != xml.size() ||
        std::fflush(file.stream.get()) != 0) {
        result.error = io_error("Cannot write GPS export");
    }
    errno = 0;
    if (std::fclose(file.stream.release()) != 0) {
        if (!result.error.empty()) result.error += "; ";
        result.error += io_error("Cannot close GPS export");
    }
    if (!result.error.empty()) {
        result.error += remove_incomplete_file(file.path);
        return result;
    }
    result.path = file.path;
    return result;
}

namespace {
bool valid_note_text(const std::string& text) {
    return !text.empty() && text.size() <= 160 &&
           std::all_of(text.begin(), text.end(), [](unsigned char ch) {
               return ch >= 32 && ch <= 126 && ch != '#';
           }) && text.find_first_not_of(' ') != std::string::npos;
}
} // namespace

FieldNote save_field_note(const std::filesystem::path& directory, const std::string& text) {
    FieldNote result;
    if (!valid_note_text(text)) {
        result.error = "Note requires 1-160 printable characters without #";
        return result;
    }
    auto file = SurveyFile::create(directory, result.error);
    if (!file) return result;
    const auto path = file->path();
    SurveyObservation observation;
    observation.recorded_at = std::chrono::system_clock::now();
    observation.source = "note";
    observation.name = "note:" + text;
    const bool written = file->append(observation, result.error);
    std::string close_error;
    const bool closed = file->close(close_error);
    file.reset();
    if (!written || !closed) {
        if (!close_error.empty()) {
            if (!result.error.empty()) result.error += "; ";
            result.error += close_error;
        }
        result.error += remove_incomplete_file(path);
        return result;
    }
    result.filename = path.filename().u8string();
    return result;
}

FieldNote read_field_note(const std::filesystem::path& directory, const std::string& filename,
                          const std::atomic_bool& cancelled) {
    FieldNote result;
    const auto timeline = read_survey_timeline_bounded(directory, filename, cancelled, 4096);
    if (!timeline.error.empty()) { result.error = timeline.error; return result; }
    if (timeline.total_rows != 1 || timeline.entries.size() != 1 ||
        timeline.entries[0].source != "note" || timeline.entries[0].name.compare(0, 5, "note:") != 0 ||
        !timeline.entries[0].gps_latitude.empty() ||
        !valid_note_text(timeline.entries[0].name.substr(5))) {
        result.error = "Invalid field note";
        return result;
    }
    result.filename = filename;
    result.recorded_utc = timeline.entries[0].recorded_utc;
    result.text = timeline.entries[0].name.substr(5);
    return result;
}

SurveyFile::SurveyFile(UniqueFile file) : file_(std::move(file)) {}

SurveyFile::~SurveyFile() = default;

std::unique_ptr<SurveyFile> SurveyFile::create(const std::filesystem::path& directory,
                                                std::string& error) {
    error = directory_error(directory);
    if (!error.empty()) return nullptr;
    const auto sessions = list_survey_files(directory);
    if (!sessions.error.empty()) {
        error = sessions.error;
        return nullptr;
    }
    if (sessions.names.size() >= kMaxSessions) {
        error = "Survey session limit reached (256); delete a session first";
        return nullptr;
    }
    auto file = open_unique_file(directory, "survey-", ".csv");
    if (!file.error.empty()) {
        error = file.error;
        return nullptr;
    }
    auto writer = std::unique_ptr<SurveyFile>(new SurveyFile(std::move(file)));
    const auto length = std::strlen(kHeader);
    errno = 0;
    if (std::fwrite(kHeader, 1, length, writer->file_.stream.get()) != length ||
        std::fflush(writer->file_.stream.get()) != 0) {
        error = io_error("Cannot write survey header");
        writer->file_.stream.reset();
        error += remove_incomplete_file(writer->file_.path);
        return nullptr;
    }
    writer->bytes_ = length;
    return writer;
}

bool SurveyFile::append(const SurveyObservation& observation, std::string& error) {
    error.clear();
    if (!file_.stream) {
        error = "Survey file is closed";
        return false;
    }
    const auto timestamp = utc(observation.recorded_at);
    if (timestamp.empty()) {
        error = "Cannot format observation timestamp";
        return false;
    }
    std::string line = "3," + csv(timestamp) + "," + csv(observation.source) + "," +
        csv(observation.name) + "," + csv(observation.address) + "," +
        csv(observation.signal) + "," + csv(observation.signal_unit) + "," +
        csv(observation.frequency_mhz) + "," + csv(observation.channel) + "," +
        csv(observation.security) + "," + csv(observation.freshness) + "," +
        (observation.scan_may_be_limited ? "true," : "false,");
    if (observation.location) {
        const auto fix_time = utc(observation.location->utc);
        if (fix_time.empty()) {
            error = "Cannot format GPS timestamp";
            return false;
        }
        line += coordinate(observation.location->latitude) + "," +
                coordinate(observation.location->longitude) + "," + csv(fix_time) + "," +
                csv(observation.location->mode);
    } else {
        line += ",,,";
    }
    line += "," + csv(observation.indicators) + "," + csv(observation.remote_id.aircraft_id) +
            "," + csv(observation.remote_id.aircraft_id_type) + ",";
    if (!observation.remote_id.aircraft_id.empty()) {
        const auto id_time = utc(observation.remote_id.id_observed_at);
        if (id_time.empty()) {
            error = "Cannot format drone ID timestamp";
            return false;
        }
        line += csv(id_time);
    }
    line += ',';
    if (observation.remote_id.position) {
        const auto position_time = utc(observation.remote_id.position->observed_at);
        if (position_time.empty()) {
            error = "Cannot format drone position timestamp";
            return false;
        }
        line += coordinate(observation.remote_id.position->latitude) + "," +
                coordinate(observation.remote_id.position->longitude) + "," +
                csv(position_time);
    } else {
        line += ",,";
    }
    line += '\n';
    if (bytes_ + line.size() > kMaxSessionBytes) {
        error = "Survey reached the 10 MiB session limit";
        return false;
    }
    errno = 0;
    if (std::fwrite(line.data(), 1, line.size(), file_.stream.get()) != line.size() ||
        std::fflush(file_.stream.get()) != 0) {
        error = io_error("Cannot write survey");
        return false;
    }
    bytes_ += line.size();
    return true;
}

bool SurveyFile::close(std::string& error) {
    error.clear();
    if (!file_.stream) return true;
    auto* stream = file_.stream.release();
    errno = 0;
    if (std::fclose(stream) != 0) {
        error = io_error("Cannot close survey");
        return false;
    }
    return true;
}

const std::filesystem::path& SurveyFile::path() const { return file_.path; }

SurveyFiles list_survey_files(const std::filesystem::path& directory) {
    SurveyFiles result;
    result.error = directory_error(directory);
    if (!result.error.empty()) return result;
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(directory, error); !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!valid_filename(name)) continue;
        const auto status = it->symlink_status(error);
        if (error) break;
        if (!std::filesystem::is_regular_file(status)) continue;
        if (result.names.size() == kMaxSessions) {
            result.error = "Too many survey sessions to list (limit 256)";
            result.names.clear();
            return result;
        }
        result.names.push_back(name);
    }
    if (error) {
        result.names.clear();
        result.error = "Cannot list surveys: " + error.message();
    } else {
        std::sort(result.names.begin(), result.names.end(), std::greater<std::string>());
    }
    return result;
}

std::string delete_survey_file(const std::filesystem::path& directory, const std::string& name) {
    auto error = directory_error(directory);
    if (!error.empty()) return error;
    if (!valid_filename(name)) return "Invalid survey filename";
    std::error_code failure;
    const auto path = directory / name;
    const auto status = std::filesystem::symlink_status(path, failure);
    if (failure) return "Cannot inspect survey: " + failure.message();
    if (!std::filesystem::is_regular_file(status)) return "Survey is not a regular file";
    if (!std::filesystem::remove(path, failure))
        return "Cannot delete survey: " + (failure ? failure.message() : "file not found");
    return {};
}

std::string delete_all_survey_files(const std::filesystem::path& directory) {
    const auto files = list_survey_files(directory);
    if (!files.error.empty()) return files.error;
    for (const auto& name : files.names) {
        const auto error = delete_survey_file(directory, name);
        if (!error.empty()) return "Deletion stopped at " + name + ": " + error;
    }
    return {};
}

} // namespace platform
