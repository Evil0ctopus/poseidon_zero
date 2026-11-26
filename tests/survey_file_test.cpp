#include "survey_file.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool check(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace platform;
    const auto directory = fs::temp_directory_path() /
        ("poseidon-survey-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    bool ok = true;
    std::string error;
    auto file = SurveyFile::create(directory, error);
    if (!file) {
        std::cerr << "Cannot create test session: " << error << '\n';
        return 1;
    }
    const auto name = file->path().filename().string();
    SurveyObservation observation;
    observation.recorded_at = std::chrono::system_clock::time_point{} +
                              std::chrono::seconds(1779626119);
    observation.source = "wifi";
    observation.name = "=name,\"quoted\"";
    observation.address = "00:11:22:33:44:55";
    observation.signal = "42";
    observation.signal_unit = "percent";
    observation.freshness = "scan";
    ok &= check(file->append(observation, error), "Could not write unlocated observation");
    observation.location = GnssFix{38.9, -92.3, observation.recorded_at, "A"};
    observation.scan_may_be_limited = true;
    observation.indicators = "name:camera;ble:remote_id_fffa";
    observation.remote_id.aircraft_id = "AIRCRAFT-123";
    observation.remote_id.aircraft_id_type = "serial";
    observation.remote_id.id_observed_at = observation.recorded_at;
    observation.remote_id.position = RemoteIdPosition{37.2, -93.1, observation.recorded_at};
    ok &= check(file->append(observation, error), "Could not write located observation");
    for (int i = 0; i < 129; ++i) {
        observation.name = "entry-" + std::to_string(i);
        ok &= check(file->append(observation, error), "Could not write timeline row");
    }
    observation.name = "multi\n\"quoted\"";
    observation.indicators = "name:camera;user-name:camera";
    ok &= check(file->append(observation, error), "Could not write multiline timeline row");
    ok &= check(file->close(error), "Could not close session");
    ok &= check(!file->append(observation, error) && !error.empty(), "Closed file accepted a row");
    std::ifstream input(directory / name, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(input)), {});
    input.close();
    ok &= check(data.find("\"'=name,\"\"quoted\"\"\"") != std::string::npos,
                "CSV quoting or spreadsheet protection failed");
    ok &= check(data.find("\"scan\",false,,,") != std::string::npos, "Missing GPS not blank");
    ok &= check(data.find("38.9000000,-92.3000000,\"2026-05-24T12:35:19.000Z\"") !=
                    std::string::npos, "GPS fields incorrect");
    ok &= check(data.find("\"scan\",true,38.9000000") != std::string::npos,
                "Scan limit warning missing");
    ok &= check(data.find("3,\"2026-05-24") != std::string::npos &&
                data.find("\"name:camera;ble:remote_id_fffa\"") != std::string::npos,
                "Versioned indicator field missing");
    ok &= check(data.find("\"AIRCRAFT-123\",\"serial\",\"2026-05-24T12:35:19.000Z\","
                          "37.2000000,-93.1000000,\"2026-05-24T12:35:19.000Z\"") !=
                    std::string::npos, "Drone fields incorrect");
    std::atomic_bool cancelled{false};
    const auto timeline = read_survey_timeline(directory, name, cancelled);
    ok &= check(timeline.error.empty() && timeline.total_rows == 132 &&
                timeline.entries.size() == 128 && timeline.entries.back().name == "multi\n\"quoted\"" &&
                timeline.entries.back().gps_latitude == "38.9000000" &&
                timeline.entries.back().gps_longitude == "-92.3000000" &&
                timeline.indicators_available &&
                survey_watchlist_match(timeline.entries.back().indicators),
                "Timeline parsing, GPS or retention limit failed");
    const auto early_name = "survey--123-0.csv";
    fs::copy_file(directory / name, directory / early_name);
    const auto early_files = list_survey_files(directory);
    ok &= check(early_files.error.empty() && early_files.names.size() == 2 &&
                read_survey_timeline(directory, early_name, cancelled).entries.size() == 128 &&
                delete_survey_file(directory, early_name).empty() &&
                !fs::exists(directory / early_name) &&
                !delete_survey_file(directory, "survey---123-0.csv").empty(),
                "Pre-1970 survey listing, reading or deletion failed");
    ok &= check(survey_watchlist_match("name:camera;user-name:camera") &&
                survey_watchlist_match("user-name:camera") &&
                !survey_watchlist_match("name:user-name:camera") &&
                !survey_watchlist_match("user-name:") &&
                !survey_watchlist_match("name:camera;ble:remote_id_fffa"),
                "Watchlist indicator parsing failed");
    const auto same_point = make_survey_plot(timeline);
    ok &= check(same_point.error.empty() && same_point.gps_rows == 128 &&
                same_point.cells[((kSurveyPlotHeight - 1) / 2) * kSurveyPlotWidth +
                                 (kSurveyPlotWidth - 1) / 2],
                "Repeated GPS points should share the center cell");
    SurveyTimeline plot_input;
    plot_input.entries = {{"", "", "", "39", "179.9", ""},
                          {"", "", "", "38", "-179.9", ""},
                          {"", "", "", "", "", ""}};
    const auto dateline_plot = make_survey_plot(plot_input);
    ok &= check(dateline_plot.error.empty() && dateline_plot.gps_rows == 2 &&
                dateline_plot.cells[0] &&
                dateline_plot.cells[kSurveyPlotWidth * kSurveyPlotHeight - 1],
                "Dateline GPS points should use the shortest longitude span with north up");
    plot_input.entries = {{"", "", "", "-20", "-0.1", ""},
                          {"", "", "", "-21", "0.1", ""}};
    const auto prime_meridian_plot = make_survey_plot(plot_input);
    ok &= check(prime_meridian_plot.error.empty() && prime_meridian_plot.gps_rows == 2 &&
                prime_meridian_plot.cells[0] &&
                prime_meridian_plot.cells[kSurveyPlotWidth * kSurveyPlotHeight - 1],
                "Southern GPS points should plot across the prime meridian");
    plot_input.entries = {{"", "", "", "0", "-180", ""},
                          {"", "", "", "0", "180", ""}};
    const auto same_meridian = make_survey_plot(plot_input);
    ok &= check(same_meridian.error.empty() && same_meridian.gps_rows == 2 &&
                same_meridian.cells[((kSurveyPlotHeight - 1) / 2) * kSurveyPlotWidth +
                                    (kSurveyPlotWidth - 1) / 2],
                "Both representations of the dateline should share a cell");
    plot_input.entries[0].gps_latitude = "91";
    ok &= check(!make_survey_plot(plot_input).error.empty(), "Invalid plot coordinate accepted");
    plot_input.entries[0].gps_latitude = "";
    ok &= check(!make_survey_plot(plot_input).error.empty(), "Incomplete plot coordinate accepted");
    plot_input.entries.clear();
    ok &= check(make_survey_plot(plot_input).gps_rows == 0, "Empty plot should have no GPS rows");
    plot_input.error = "Timeline failed";
    ok &= check(make_survey_plot(plot_input).error == plot_input.error,
                "Plot should propagate timeline errors");
    const auto center = survey_map_tile({"", "", "", "0", "0", ""}, 1);
    ok &= check(center.error.empty() && center.x == 1 && center.y == 1 &&
                center.pixel_x == 0 && center.pixel_y == 0,
                "Web Mercator equator tile should start at the expected corner");
    const auto quarter = survey_map_tile({"", "", "", "0", "-90", ""}, 0);
    ok &= check(quarter.error.empty() && quarter.x == 0 && quarter.y == 0 &&
                quarter.pixel_x == 64 && quarter.pixel_y == 128,
                "Map marker should use the position within the tile");
    const auto west = survey_map_tile({"", "", "", "0", "-180", ""}, 2);
    const auto east = survey_map_tile({"", "", "", "0", "180", ""}, 2);
    ok &= check(west.error.empty() && east.error.empty() && west.x == 0 &&
                east.x == west.x && east.pixel_x == west.pixel_x,
                "Both dateline representations should select the same tile");
    const auto north = survey_map_tile({"", "", "", "85.0511287798066", "0", ""}, 18);
    ok &= check(north.error.empty() && north.y == 0 && north.pixel_y == 0,
                "Web Mercator northern edge should use the first row");
    const auto south = survey_map_tile({"", "", "", "-85.0511287798066", "0", ""}, 18);
    ok &= check(south.error.empty() && south.y == 262143 && south.pixel_y == 255,
                "Web Mercator southern edge should stay within the last row");
    ok &= check(!survey_map_tile({"", "", "", "86", "0", ""}, 14).error.empty() &&
                !survey_map_tile({"", "", "", "", "0", ""}, 14).error.empty() &&
                !survey_map_tile({"", "", "", "0", "0", ""}, 19).error.empty(),
                "Out-of-range or incomplete map input accepted");
    const auto attribution_path = directory / "attribution.txt";
    ok &= check(!read_survey_map_attribution(attribution_path).error.empty(),
                "Missing map attribution accepted");
    {
        std::ofstream out(attribution_path, std::ios::binary);
        out << "\xc2\xa9" " Example map contributors\r\n";
    }
    const auto attribution = read_survey_map_attribution(attribution_path);
    ok &= check(attribution.error.empty() &&
                attribution.text == "\xc2\xa9" " Example map contributors" &&
                !attribution.glyphs.empty() && attribution.glyphs.front() == 0xa9,
                "UTF-8 map attribution did not round-trip");
    for (const std::string& invalid : {std::string(81, 'x'), std::string("first\nsecond"),
                                       std::string("\xc0\xaf"), std::string()}) {
        {
            std::ofstream out(attribution_path, std::ios::binary | std::ios::trunc);
            out << invalid;
        }
        ok &= check(!read_survey_map_attribution(attribution_path).error.empty(),
                    "Invalid map attribution accepted");
    }
    {
        std::ofstream out(attribution_path, std::ios::binary | std::ios::trunc);
        out << "Example map";
    }
    const auto attribution_link = directory / "attribution-link.txt";
    std::error_code link_error;
    fs::create_symlink(attribution_path, attribution_link, link_error);
    if (!link_error) {
        ok &= check(!read_survey_map_attribution(attribution_link).error.empty(),
                    "Map attribution followed a symlink");
        fs::remove(attribution_link);
    }
#if !defined(_WIN32)
    else {
        ok &= check(false, "Could not create map attribution symlink");
    }
#endif
    fs::remove(attribution_path);
    const auto gpx_directory = directory / "exports";
    const auto gpx = export_survey_gpx(gpx_directory, timeline);
    ok &= check(gpx.error.empty() && gpx.points == 128 && fs::exists(gpx.path),
                "Could not export retained receiver positions");
    if (gpx.error.empty()) {
        std::ifstream gpx_input(gpx.path, std::ios::binary);
        const std::string gpx_data((std::istreambuf_iterator<char>(gpx_input)), {});
        const std::string point = "  <wpt lat=\"38.9000000\" lon=\"-92.3000000\"/>\n";
        std::string expected = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                               "<gpx version=\"1.1\" creator=\"POSEIDON\" "
                               "xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
        for (int i = 0; i < 128; ++i) expected += point;
        expected += "</gpx>\n";
        ok &= check(gpx_data == expected, "GPX content contains incorrect or extra data");
        gpx_input.close();
        fs::remove(gpx.path);
    }
    SurveyTimeline invalid_gpx;
    invalid_gpx.entries = {{"", "wifi", "private name", "91", "0", ""}};
    ok &= check(!export_survey_gpx(gpx_directory, invalid_gpx).error.empty() &&
                fs::is_empty(gpx_directory), "Invalid GPS export created a file");
    invalid_gpx.entries[0].gps_latitude.clear();
    invalid_gpx.entries[0].gps_longitude.clear();
    ok &= check(!export_survey_gpx(gpx_directory, invalid_gpx).error.empty() &&
                fs::is_empty(gpx_directory), "Empty GPS export created a file");
    invalid_gpx.entries[0].gps_latitude = "1";
    ok &= check(!export_survey_gpx(gpx_directory, invalid_gpx).error.empty() &&
                fs::is_empty(gpx_directory), "Incomplete GPS export accepted");
    invalid_gpx.entries.resize(129);
    ok &= check(!export_survey_gpx(gpx_directory, invalid_gpx).error.empty() &&
                fs::is_empty(gpx_directory), "Oversized GPS export accepted");
    fs::remove(gpx_directory);
    const auto notes_directory = directory / "field-notes";
    const auto note1 = save_field_note(notes_directory, "=-1, \"quoted\" and 'text'");
    const auto note2 = save_field_note(notes_directory, "Second observation");
    ok &= check(note1.error.empty() && note2.error.empty() && note1.filename != note2.filename,
                "Could not create independent field notes");
    const auto loaded_note = read_field_note(notes_directory, note1.filename, cancelled);
    ok &= check(loaded_note.error.empty() && loaded_note.text == "=-1, \"quoted\" and 'text'" &&
                !loaded_note.recorded_utc.empty(), "Field note did not round-trip");
    ok &= check(!save_field_note(notes_directory, "   ").error.empty() &&
                !save_field_note(notes_directory, "line\nbreak").error.empty() &&
                !save_field_note(notes_directory, "tag #1").error.empty() &&
                !save_field_note(notes_directory, std::string(161, 'x')).error.empty(),
                "Invalid field note text accepted");
    ok &= check(!read_field_note(notes_directory, "../outside.csv", cancelled).error.empty(),
                "Field note traversal accepted");
    cancelled = true;
    ok &= check(!read_field_note(notes_directory, note1.filename, cancelled).error.empty(),
                "Cancelled field note read accepted");
    cancelled = false;
    ok &= check(delete_survey_file(notes_directory, note1.filename).empty() &&
                !std::filesystem::exists(notes_directory / note1.filename) &&
                read_field_note(notes_directory, note2.filename, cancelled).text == "Second observation",
                "Selective field note deletion failed");
    {
        std::ofstream oversized(notes_directory / note2.filename, std::ios::app | std::ios::binary);
        oversized << std::string(4097, 'x');
    }
    ok &= check(!read_field_note(notes_directory, note2.filename, cancelled).error.empty(),
                "Oversized field note accepted");
    auto other_file = SurveyFile::create(notes_directory, error);
    if (!other_file) {
        std::cerr << "Could not create non-note fixture: " << error << '\n';
        return 1;
    }
    const auto other_name = other_file->path().filename().string();
    observation.source = "wifi";
    observation.name = "note:not a note";
    ok &= check(other_file->append(observation, error) && other_file->close(error),
                "Could not write non-note fixture");
    ok &= check(!read_field_note(notes_directory, other_name, cancelled).error.empty(),
                "Non-note survey was accepted as a field note");
    ok &= check(delete_survey_file(notes_directory, other_name).empty(),
                "Non-note fixture cleanup failed");
    const auto note_link = directory / "notes-link";
    std::error_code note_link_error;
    fs::create_directory_symlink(notes_directory, note_link, note_link_error);
    if (!note_link_error) {
        ok &= check(!save_field_note(note_link, "must not follow link").error.empty(),
                    "Field note directory link accepted");
        fs::remove(note_link, note_link_error);
        ok &= check(!note_link_error, "Field note directory link cleanup failed");
    }
#if !defined(_WIN32)
    else {
        ok &= check(false, "Could not create field note directory link");
    }
#endif
    ok &= check(delete_survey_file(notes_directory, note2.filename).empty(),
                "Field note cleanup failed");
    std::error_code notes_cleanup;
    fs::remove(notes_directory, notes_cleanup);
    ok &= check(!notes_cleanup, "Field note test directory cleanup failed");
    cancelled = true;
    ok &= check(!read_survey_timeline(directory, name, cancelled).error.empty(),
                "Cancelled timeline accepted");
    cancelled = false;
    ok &= check(!read_survey_timeline(directory, "../outside.csv", cancelled).error.empty(),
                "Timeline traversal accepted");
    const auto older = directory / "survey-999-0.csv";
    {
        std::ofstream legacy(older, std::ios::binary);
        legacy << "version,recorded_utc,source,name,gps_latitude,gps_longitude\n"
                  "1,\"time\",\"ble\",\"old\",,\n";
    }
    const auto legacy = read_survey_timeline(directory, older.filename().string(), cancelled);
    ok &= check(legacy.error.empty() && !legacy.indicators_available && legacy.entries.size() == 1 &&
                legacy.entries[0].name == "old" && legacy.entries[0].gps_latitude.empty(),
                "Earlier timeline schema failed");
    {
        std::ofstream malformed(older, std::ios::binary | std::ios::trunc);
        malformed << "version,recorded_utc,source,name,gps_latitude,gps_longitude\n"
                     "3,time,ble,name,91,0\n";
    }
    ok &= check(!read_survey_timeline(directory, older.filename().string(), cancelled).error.empty(),
                "Invalid timeline coordinates accepted");
    {
        std::ofstream malformed(older, std::ios::binary | std::ios::trunc);
        malformed << "version,recorded_utc,source,name,gps_latitude,gps_longitude\n"
                     "3,time,ble,\"unterminated,,";
    }
    ok &= check(!read_survey_timeline(directory, older.filename().string(), cancelled).error.empty(),
                "Malformed timeline CSV accepted");
    fs::remove(older);
    ok &= check(list_survey_files(directory).names.size() == 1, "Session not listed");
    ok &= check(!delete_survey_file(directory, "../outside.csv").empty(), "Traversal accepted");
    std::ofstream(directory / "unrelated.txt") << "keep";
#if !defined(_WIN32)
    std::error_code symlink_error;
    const auto link = directory / "survey-123-45.csv";
    fs::create_symlink(directory / "unrelated.txt", link, symlink_error);
    ok &= check(!symlink_error, "Could not create test symlink");
    if (!symlink_error) {
        ok &= check(list_survey_files(directory).names.size() == 1, "Symlink listed as a session");
        ok &= check(!delete_survey_file(directory, link.filename().string()).empty(),
                    "Symlink deletion accepted");
        ok &= check(!read_survey_timeline(directory, link.filename().string(), cancelled).error.empty(),
                    "Timeline symlink accepted");
    }
#endif
    ok &= check(delete_all_survey_files(directory).empty(), "Delete all failed");
    ok &= check(fs::exists(directory / "unrelated.txt") && !fs::exists(directory / name),
                "Delete all affected wrong files");
    std::error_code cleanup;
#if !defined(_WIN32)
    if (!symlink_error) fs::remove(link, cleanup);
#endif
    fs::remove(directory / "unrelated.txt", cleanup);
    fs::remove(directory, cleanup);
    return ok ? 0 : 1;
}
