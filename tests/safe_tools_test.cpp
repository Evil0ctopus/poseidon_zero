#include "gallery_image.h"
#include "camera_capture.h"
#include "bluetooth_gatt.h"
#include "expansion_inventory.h"
#include "feature_catalog.h"
#include "ir_receive.h"
#include "linux_network.h"
#include "port_scan.h"
#include "safe_tools.h"
#include "tool_status.h"
#include "wifi_scan.h"

#include <cstdio>
#include <jpeglib.h>
#include <png.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace {

bool expect_result(const std::string& expression, double expected) {
    bool valid = false;
    const double result = platform::calculate_expression(expression, valid);
    if (!valid || std::abs(result - expected) > 1e-9) {
        std::cerr << "Expected " << expression << " to equal " << expected
                  << ", got " << result << " (valid=" << valid << ")\n";
        return false;
    }
    return true;
}

bool expect_invalid(const std::string& expression) {
    bool valid = true;
    platform::calculate_expression(expression, valid);
    if (valid) {
        std::cerr << "Expected invalid expression: " << expression << '\n';
        return false;
    }
    return true;
}

bool expect_string(const std::string& actual,
                   const std::string& expected,
                   const std::string& description) {
    if (actual != expected) {
        std::cerr << "Expected " << description << " to be \"" << expected
                  << "\", got \"" << actual << "\"\n";
        return false;
    }
    return true;
}

bool write_jpeg(const std::filesystem::path& path, unsigned int width) {
#if defined(_WIN32)
    FILE* file = _wfopen(path.c_str(), L"wb");
#else
    FILE* file = std::fopen(path.c_str(), "wb");
#endif
    if (!file) {
        return false;
    }
    jpeg_compress_struct encoder{};
    jpeg_error_mgr error{};
    encoder.err = jpeg_std_error(&error);
    jpeg_create_compress(&encoder);
    jpeg_stdio_dest(&encoder, file);
    encoder.image_width = width;
    encoder.image_height = 1;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, 100, TRUE);
    jpeg_start_compress(&encoder, TRUE);
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * 3U, 0);
    for (unsigned int x = 0; x < width; ++x) {
        pixels[x * 3U] = 255;
    }
    JSAMPROW row = pixels.data();
    jpeg_write_scanlines(&encoder, &row, 1);
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    return std::fclose(file) == 0;
}

} // namespace

int main() {
    bool passed = true;
    std::vector<platform::BluetoothDevice> devices(3);
    devices[0].address = "first";
    devices[1].address = "indicator";
    devices[1].tracker_indicator = true;
    devices[2].address = "last";
    const auto indicators = platform::tracker_indicator_devices(devices);
    if (indicators.size() != 1 || indicators[0].address != "indicator" ||
        !platform::tracker_indicator_devices({}).empty()) {
        std::cerr << "Tracker results must contain only FCB2 indicator observations\n";
        passed = false;
    }
    if (!platform::is_ethernet_interface_type(1, false) ||
        platform::is_ethernet_interface_type(1, true) ||
        platform::is_ethernet_interface_type(772, false)) {
        std::cerr << "Ethernet classification must exclude wireless and non-Ethernet types\n";
        passed = false;
    }
    const auto internet = platform::check_internet();
    if (internet.connected || internet.detail.empty()) {
        std::cerr << "Desktop internet check must report unsupported without a network request\n";
        passed = false;
    }
    if (platform::classify_expansion_node("spidev0.0") !=
            platform::ExpansionNodeKind::Spi ||
        platform::classify_expansion_node("gpiochip12") !=
            platform::ExpansionNodeKind::Gpio ||
        platform::classify_expansion_node("spidev0") !=
            platform::ExpansionNodeKind::None ||
        platform::classify_expansion_node("spidev0.x") !=
            platform::ExpansionNodeKind::None ||
        platform::classify_expansion_node("gpiochip") !=
            platform::ExpansionNodeKind::None ||
        platform::classify_expansion_node("ttyUSB0") !=
            platform::ExpansionNodeKind::None) {
        std::cerr << "Expansion inventory must recognize only SPI and GPIO device nodes\n";
        passed = false;
    }
    if (platform::read_expansion_bus_inventory().error.empty()) {
        std::cerr << "Desktop expansion inventory must report unsupported hardware\n";
        passed = false;
    }
    platform::St25r3916Identity nfc_identity;
    if (!platform::decode_st25r3916_identity(0x2A, nfc_identity) ||
        nfc_identity.value != 0x2A ||
        nfc_identity.revision != 2 ||
        platform::decode_st25r3916_identity(0x28, nfc_identity) ||
        platform::decode_st25r3916_identity(0xFF, nfc_identity)) {
        std::cerr << "ST25R3916 identity decoding must accept a valid revision and reject invalid IDs\n";
        passed = false;
    }
    const auto nfc_probe = platform::read_cap_nfc_identity();
    if (nfc_probe.detected || nfc_probe.error.empty()) {
        std::cerr << "Desktop NFC readiness must report unsupported hardware\n";
        passed = false;
    }
    const auto expansion_features = platform::expansion_features();
    for (const std::size_t index : {std::size_t{4}, std::size_t{5}}) {
        const auto& feature = expansion_features[index];
        const std::string description = feature.description;
        if (feature.available ||
            description.find("Hydra RF 924 Pro") == std::string::npos ||
            description.find("18-pin") == std::string::npos ||
            description.find("Zero adapter") == std::string::npos) {
            std::cerr << "Hydra radio entries must remain disabled and identify the unverified adapter\n";
            passed = false;
        }
    }
    const auto& nfc_feature = expansion_features[7];
    const std::string nfc_description = nfc_feature.description;
    if (nfc_feature.available ||
        nfc_description.find("identity") == std::string::npos ||
        nfc_description.find("no NFC field") == std::string::npos) {
        std::cerr << "Desktop NFC readiness must remain unavailable and describe its read-only scope\n";
        passed = false;
    }
    const auto parsed_ports = platform::parse_port_list("443,80-82,80");
    const auto all_ports = platform::parse_port_list("1-65535");
    if (!parsed_ports.error.empty() || parsed_ports.ports != std::vector<uint16_t>{80, 81, 82, 443} ||
        platform::format_port_list(parsed_ports.ports) != "80-82,443" ||
        !all_ports.error.empty() || all_ports.ports.size() != 65535 ||
        platform::format_port_list(all_ports.ports) != "1-65535" ||
        platform::parse_port_list("0").error.empty() ||
        platform::parse_port_list("5-2").error.empty() ||
        platform::parse_port_list("1,").error.empty() ||
        platform::parse_port_list("65536").error.empty()) {
        std::cerr << "Port ranges must validate and deduplicate\n";
        passed = false;
    }
    const std::atomic_bool scan_running{false};
    const auto subnet = platform::resolve_port_targets("192.0.2.0/30", scan_running);
    if (!subnet.error.empty() ||
        subnet.addresses != std::vector<std::string>{"192.0.2.1", "192.0.2.2"} ||
        platform::resolve_port_targets("192.0.2.0/23", scan_running).error.empty() ||
        platform::resolve_port_targets("224.0.0.1", scan_running).error.empty() ||
        platform::resolve_port_targets("::ffff:192.0.2.1", scan_running).error.empty() ||
        platform::resolve_port_targets("ff02::1", scan_running).error.empty() ||
        platform::resolve_port_targets("2001:db8::/119", scan_running).error.empty() ||
        platform::resolve_port_targets("192.0.2.999", scan_running).error.empty()) {
        std::cerr << "Targets must be unicast and subnet size bounded\n";
        passed = false;
    }
    const std::atomic_bool camera_running{false};
    if (platform::capture_camera(camera_running).error.empty()) {
        std::cerr << "Desktop camera must report unsupported\n";
        passed = false;
    }
    platform::IrReceiveResult ir;
    platform::append_ir_sample(ir, 500U);
    platform::append_ir_sample(ir, 0x02000000U | 38000U);
    platform::append_ir_sample(ir, 0x01000000U | 9000U);
    platform::append_ir_sample(ir, 4500U);
    if (ir.samples.size() != 2 || !ir.samples[0].pulse || ir.samples[0].duration_us != 9000 ||
        ir.samples[1].pulse || ir.samples[1].duration_us != 4500 ||
        platform::append_ir_sample(ir, 0x03000000U) != platform::IrSampleStatus::Complete ||
        platform::append_ir_sample(ir, 0x04000000U) != platform::IrSampleStatus::Overflow) {
        std::cerr << "IR samples must retain pulse and space timings and detect end or overflow\n";
        passed = false;
    }
    platform::IrReceiveResult long_ir;
    for (int i = 0; i < 128; ++i) platform::append_ir_sample(long_ir, 0x01000001U);
    if (platform::append_ir_sample(long_ir, 0x01000001U) != platform::IrSampleStatus::Complete ||
        long_ir.samples.size() != 128 || !long_ir.truncated) {
        std::cerr << "IR receive must bound and mark truncated samples\n";
        passed = false;
    }
    std::atomic_bool ir_cancelled{false};
    const auto ir_receiver = platform::inspect_ir_receiver();
    if (ir_receiver.available || ir_receiver.error.empty()) {
        std::cerr << "Desktop IR receiver diagnostics must report unsupported\n";
        passed = false;
    }
    if (platform::receive_ir(ir_cancelled).error.empty()) {
        std::cerr << "Desktop IR receive must report unsupported\n";
        passed = false;
    }
    ir_cancelled = true;
    const auto cancelled_ir = platform::receive_ir(ir_cancelled);
    if (!cancelled_ir.error.empty() || !cancelled_ir.samples.empty()) {
        std::cerr << "Cancelled IR receive must not report data or an error\n";
        passed = false;
    }
    if (platform::wifi_channel(2412) != 1 || platform::wifi_channel(2484) != 14 ||
        platform::wifi_channel(5180) != 36 || platform::wifi_channel(5935) != 2 ||
        platform::wifi_channel(5955) != 1 || platform::wifi_channel(58320) != 1 ||
        platform::wifi_channel(2413) != -1 || platform::wifi_channel(0) != -1) {
        std::cerr << "Wi-Fi frequencies must map to the correct channel or report unknown\n";
        passed = false;
    }
    platform::WifiScanResult freshness;
    platform::WifiScanNetwork fresh_ap;
    fresh_ap.last_seen_boottime = 101;
    if (platform::wifi_seen_during_scan(freshness, fresh_ap)) {
        std::cerr << "Unknown scan start must not count as a fresh observation\n";
        passed = false;
    }
    freshness.scan_started_boottime = 100;
    if (!platform::wifi_seen_during_scan(freshness, fresh_ap)) {
        std::cerr << "AP seen after scan start should be considered fresh\n";
        passed = false;
    }
    fresh_ap.last_seen_boottime = 100;
    if (platform::wifi_seen_during_scan(freshness, fresh_ap)) {
        std::cerr << "Ambiguous same-second AP result must be excluded\n";
        passed = false;
    }
    platform::WifiScanResult baseline_ap;
    baseline_ap.scan_started_boottime = 100;
    platform::WifiScanNetwork first;
    first.address = "AA:BB:CC:DD:EE:01";
    first.name = "Example";
    first.frequency_mhz = 2412;
    first.last_seen_boottime = 101;
    baseline_ap.networks.push_back(first);
    auto later_ap = baseline_ap;
    later_ap.scan_started_boottime = 200;
    later_ap.networks[0].last_seen_boottime = 201;
    later_ap.networks[0].frequency_mhz = 2437;
    auto added_ap = first;
    added_ap.address = "aa:bb:cc:dd:ee:02";
    added_ap.last_seen_boottime = 201;
    later_ap.networks.push_back(added_ap);
    const auto changes = platform::compare_wifi_observations(baseline_ap, later_ap);
    if (!changes.error.empty() || changes.changed != 1 || changes.new_addresses != 1 ||
        changes.not_seen != 0 || changes.details.size() != 2) {
        std::cerr << "AP monitor must compare fresh address observations\n";
        passed = false;
    }
    later_ap.networks[0].last_seen_boottime = 200;
    const auto stale = platform::compare_wifi_observations(baseline_ap, later_ap);
    if (stale.not_seen != 1 || stale.skipped != 1) {
        std::cerr << "Stale AP observations must not count as present\n";
        passed = false;
    }
    auto duplicate_baseline = baseline_ap;
    duplicate_baseline.networks.push_back(duplicate_baseline.networks[0]);
    auto duplicate_current = later_ap;
    duplicate_current.networks[0].last_seen_boottime = 201;
    const auto ambiguous = platform::compare_wifi_observations(duplicate_baseline, duplicate_current);
    if (ambiguous.new_addresses != 1 || ambiguous.changed != 0 ||
        ambiguous.not_seen != 0 || ambiguous.baseline_count != 0 ||
        ambiguous.current_count != 1) {
        std::cerr << "Duplicate AP addresses must not create spurious comparisons\n";
        passed = false;
    }
    later_ap.scan_started_boottime = -1;
    if (platform::compare_wifi_observations(baseline_ap, later_ap).error.empty()) {
        std::cerr << "Missing scan freshness must be reported\n";
        passed = false;
    }
    std::vector<platform::WifiScanNetwork> channel_networks(6);
    channel_networks[0].frequency_mhz = 2412;
    channel_networks[1].frequency_mhz = 2412;
    channel_networks[2].frequency_mhz = 2437;
    channel_networks[3].frequency_mhz = 5180;
    channel_networks[4].frequency_mhz = 5955;
    const auto channel_summary = platform::summarize_wifi_channels(channel_networks);
    if (channel_summary.size() != 4 ||
        channel_summary[0].band != "2.4 GHz" || channel_summary[0].channel != 1 ||
        channel_summary[0].access_points != 2 ||
        channel_summary[1].band != "2.4 GHz" || channel_summary[1].channel != 6 ||
        channel_summary[2].band != "5 GHz" || channel_summary[2].channel != 36 ||
        channel_summary[3].band != "6 GHz" || channel_summary[3].channel != 1) {
        std::cerr << "Channel survey must count visible APs by band and omit unknown channels\n";
        passed = false;
    }
    platform::WifiScanNetwork security_fixture;
    if (platform::wifi_hardening_advice(security_fixture).find("Open network") == std::string::npos) {
        std::cerr << "Open Wi-Fi advice failed\n";
        passed = false;
    }
    if (platform::wifi_security(security_fixture) != platform::WifiSecurity::Open ||
        !platform::valid_wifi_password(platform::WifiSecurity::Open, "")) {
        std::cerr << "Open Wi-Fi must not require a password\n";
        passed = false;
    }
    security_fixture.secured = true;
    security_fixture.rsn_flags = 0x100;
    if (platform::wifi_security(security_fixture) != platform::WifiSecurity::WpaPsk ||
        !platform::valid_wifi_password(platform::WifiSecurity::WpaPsk, "password") ||
        platform::valid_wifi_password(platform::WifiSecurity::WpaPsk, "short") ||
        !platform::valid_wifi_password(platform::WifiSecurity::WpaPsk, std::string(64, 'a')) ||
        platform::valid_wifi_password(platform::WifiSecurity::WpaPsk, std::string(64, 'z'))) {
        std::cerr << "WPA password validation failed\n";
        passed = false;
    }
    security_fixture.rsn_flags = 0x500;
    if (platform::wifi_hardening_advice(security_fixture).find("transition") == std::string::npos) {
        std::cerr << "Transition-mode advice failed\n";
        passed = false;
    }
    if (platform::wifi_security(security_fixture) != platform::WifiSecurity::Sae ||
        !platform::valid_wifi_password(platform::WifiSecurity::Sae, "x") ||
        platform::valid_wifi_password(platform::WifiSecurity::Sae, std::string(64, 'a'))) {
        std::cerr << "WPA3 must be preferred when advertised\n";
        passed = false;
    }
    security_fixture.rsn_flags = 0x200;
    if (platform::wifi_hardening_advice(security_fixture).find("Enterprise") == std::string::npos) {
        std::cerr << "Enterprise advice failed\n";
        passed = false;
    }
    if (platform::wifi_security(security_fixture) != platform::WifiSecurity::Unsupported) {
        std::cerr << "Enterprise authentication must not be treated as a personal password\n";
        passed = false;
    }
    security_fixture.rsn_flags = 0;
    security_fixture.wpa_flags = 0x100;
    if (platform::wifi_hardening_advice(security_fixture).find("Legacy WPA") == std::string::npos) {
        std::cerr << "Legacy WPA advice failed\n";
        passed = false;
    }
    security_fixture.wpa_flags = 0;
    if (platform::wifi_hardening_advice(security_fixture).find("not determined") == std::string::npos) {
        std::cerr << "Unknown security must not be presented as open or secure\n";
        passed = false;
    }
    std::atomic_bool scan_cancelled{false};
    const auto desktop_scan = platform::scan_wifi(scan_cancelled);
    if (desktop_scan.error.empty() || !desktop_scan.networks.empty()) {
        std::cerr << "Desktop Wi-Fi scan must report unsupported, not simulated networks\n";
        passed = false;
    }
    scan_cancelled = true;
    const auto cancelled_scan = platform::scan_wifi(scan_cancelled);
    if (!cancelled_scan.error.empty() || !cancelled_scan.networks.empty()) {
        std::cerr << "Cancelled Wi-Fi scan must not return results or an error\n";
        passed = false;
    }
    platform::NetworkInterfacesResult adapter_fixture;
    adapter_fixture.interfaces = {
        {"Wi-Fi", "192.0.2.1", true, true, false},
        {"Ethernet", "192.0.2.2", false, true, true},
        {"USB Ethernet", "", false, false, true},
    };
    const auto ethernet = platform::filter_ethernet_interfaces(std::move(adapter_fixture));
    if (!ethernet.error.empty() || ethernet.interfaces.size() != 2 ||
        ethernet.interfaces[0].name != "Ethernet" || ethernet.interfaces[0].connected ||
        ethernet.interfaces[1].name != "USB Ethernet" || ethernet.interfaces[1].state_known) {
        std::cerr << "Ethernet view must exclude Wi-Fi and preserve adapter state\n";
        passed = false;
    }
    const std::string route_header =
        "Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT\n";
    std::istringstream routes(
        route_header +
        "wlan0 00000000 0101A8C0 0003 0 0 50 00000000 0 0 0\n"
        "eth0 00000000 0100000A 0003 0 0 10 00000000 0 0 0\n"
        "usb0 00000000 0102A8C0 0002 0 0 1 00000000 0 0 0\n"
        "vpn0 00000000 0102A8C0 0003 0 0 1 000000FF 0 0 0\n");
    passed &= expect_string(platform::parse_default_gateway(routes), "10.0.0.1",
                            "lowest-metric active gateway");
    std::istringstream malformed(route_header + "eth0 00000000 NOTHEX 0003 0 0 0 00000000\n");
    passed &= expect_string(platform::parse_default_gateway(malformed), "Gateway data invalid",
                            "malformed gateway");
    std::istringstream no_gateway(route_header + "eth0 00000000 00000000 0001 0 0 0 00000000\n");
    passed &= expect_string(platform::parse_default_gateway(no_gateway), "No gateway",
                            "direct route without gateway");
    std::istringstream direct_preferred(
        route_header +
        "wlan0 00000000 0101A8C0 0003 0 0 50 00000000\n"
        "ppp0 00000000 00000000 0001 0 0 5 00000000\n");
    passed &= expect_string(platform::parse_default_gateway(direct_preferred), "No gateway",
                            "preferred direct route");
    std::istringstream missing_routes("");
    passed &= expect_string(platform::parse_default_gateway(missing_routes),
                            "Route table unavailable", "unavailable route table");
    const std::string arp_header =
        "IP address       HW type     Flags       HW address            Mask     Device\n";
    std::istringstream arp(arp_header +
        "192.168.1.1 0x1 0x2 aa:bb:cc:dd:ee:ff * wlan0\n");
    const auto arp_result = platform::parse_arp_table(arp);
    if (!arp_result.error.empty() || arp_result.truncated || arp_result.entries.size() != 1 ||
        arp_result.entries[0].address != "192.168.1.1" ||
        arp_result.entries[0].hardware_address != "aa:bb:cc:dd:ee:ff" ||
        arp_result.entries[0].interface_name != "wlan0") {
        std::cerr << "ARP table should expose address, hardware address, and interface\n";
        passed = false;
    }
    std::istringstream empty_arp(arp_header);
    const auto empty_arp_result = platform::parse_arp_table(empty_arp);
    if (!empty_arp_result.error.empty() || !empty_arp_result.entries.empty()) {
        std::cerr << "An empty ARP table should be distinguished from an error\n";
        passed = false;
    }
    std::istringstream invalid_arp(arp_header + "invalid row\n");
    std::istringstream unavailable_arp("");
    if (platform::parse_arp_table(invalid_arp).error.empty() ||
        platform::parse_arp_table(unavailable_arp).error.empty()) {
        std::cerr << "Malformed or unavailable ARP tables must report an error\n";
        passed = false;
    }
    std::string many_arp = arp_header;
    for (int i = 0; i < 33; ++i) {
        many_arp += "192.168.1.1 0x1 0x2 aa:bb:cc:dd:ee:ff * wlan0\n";
    }
    std::istringstream large_arp(many_arp);
    const auto limited_arp = platform::parse_arp_table(large_arp);
    if (!limited_arp.error.empty() || !limited_arp.truncated || limited_arp.entries.size() != 32) {
        std::cerr << "ARP listings must report truncation after 32 entries\n";
        passed = false;
    }
#if defined(_WIN32)
    if (platform::default_gateway() == "Desktop emulator") {
        std::cerr << "Host gateway lookup must not return simulated data\n";
        passed = false;
    }
    const auto host_interfaces = platform::list_network_interfaces();
    if (!host_interfaces.error.empty() && !host_interfaces.interfaces.empty()) {
        std::cerr << "Failed host enumeration must not return partial interfaces\n";
        passed = false;
    }
    for (const auto& interface : host_interfaces.interfaces) {
        if (interface.name.empty() || interface.name == "emulator") {
            std::cerr << "Host interface listing must not use simulated adapters\n";
            passed = false;
        }
    }
    const auto host_arp = platform::arp_entries();
    if (!host_arp.error.empty() && !host_arp.entries.empty()) {
        std::cerr << "Failed host ARP enumeration must not return partial entries\n";
        passed = false;
    }
    for (const auto& entry : host_arp.entries) {
        if (entry.address.empty() || entry.hardware_address.empty() ||
            entry.interface_name.empty()) {
            std::cerr << "Host ARP entries must include address and interface information\n";
            passed = false;
        }
    }
#endif
    const auto unique_part = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = std::filesystem::temp_directory_path() /
                      ("poseidon-files-" + std::to_string(unique_part));
    std::error_code error;
    if (!std::filesystem::create_directory(temp, error)) {
        std::cerr << "Could not create temporary directory: " << error.message() << '\n';
        return 1;
    }
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{temp};
    std::filesystem::create_directory(temp / "folder", error);
    {
        std::ofstream output(temp / "sample.txt");
        output << "hello";
        if (!output || error) {
            std::cerr << "Could not create browser test entries\n";
            return 1;
        }
    }
    const auto listing = platform::read_directory(temp.u8string());
    if (!listing.error.empty() || listing.truncated || listing.entries.size() != 2 ||
        !listing.entries[0].directory || listing.entries[0].name != "folder" ||
        listing.entries[1].directory || listing.entries[1].name != "sample.txt" ||
        listing.entries[1].size != 5) {
        std::cerr << "Directory listing should show directories and file sizes\n";
        passed = false;
    }
    if (platform::read_directory((temp / "missing").u8string()).error.empty()) {
        std::cerr << "Missing directories must report an error\n";
        passed = false;
    }
    for (int i = 0; i < 64; ++i) {
        std::ofstream output(temp / ("entry-" + std::to_string(i)));
        if (!output) {
            std::cerr << "Could not create truncation test entries\n";
            return 1;
        }
    }
    const auto limited = platform::read_directory(temp.u8string());
    if (!limited.error.empty() || !limited.truncated || limited.entries.size() != 64) {
        std::cerr << "Large directories must report that the listing is limited\n";
        passed = false;
    }
    const auto next_page = platform::read_directory(temp.u8string(), platform::kDirectoryPageSize);
    if (!next_page.error.empty() || next_page.truncated || next_page.entries.size() != 2) {
        std::cerr << "Later directory pages should expose the remaining entries\n";
        passed = false;
    }
    std::set<std::string> page_names;
    for (const auto& entry : limited.entries) {
        page_names.insert(entry.name);
    }
    for (const auto& entry : next_page.entries) {
        page_names.insert(entry.name);
    }
    if (page_names.size() != 66 || page_names.count("folder") != 1 ||
        page_names.count("sample.txt") != 1) {
        std::cerr << "Directory pages must contain each entry exactly once\n";
        passed = false;
    }
    const std::atomic_bool cancelled{true};
    const auto cancelled_listing = platform::read_directory(temp.u8string(), 0, &cancelled);
    if (cancelled_listing.error.empty() || !cancelled_listing.entries.empty()) {
        std::cerr << "Canceled directory reads must not report a successful listing\n";
        passed = false;
    }
    const auto saved_ir = platform::save_ir_capture(temp, ir);
    const auto saved_ir_again = platform::save_ir_capture(temp, ir);
    if (!saved_ir.error.empty() || !saved_ir_again.error.empty() ||
        saved_ir.path == saved_ir_again.path) {
        std::cerr << "IR capture saves must create unique files\n";
        passed = false;
    } else {
        std::ifstream input(saved_ir.path, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(input), {}};
        if (!input || contents != "POSEIDON_IR_V1\ncarrier_hz=unknown\npulse 9000\nspace 4500\n") {
            std::cerr << "IR capture must preserve timings and unknown carrier\n";
            passed = false;
        }
    }
    if (platform::save_ir_capture(temp, long_ir).error.empty() ||
        platform::save_ir_capture(temp, {}).error.empty() ||
        platform::save_ir_capture(temp / "missing", ir).error.empty()) {
        std::cerr << "Incomplete IR captures and invalid destinations must be rejected\n";
        passed = false;
    }
    const auto png_path = temp / "image.png";
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = 2;
    png.height = 1;
    png.format = PNG_FORMAT_BGRA;
    const uint8_t expected_pixels[] = {0, 0, 255, 255, 0, 255, 0, 128};
    FILE* png_file = nullptr;
#if defined(_WIN32)
    png_file = _wfopen(png_path.c_str(), L"wb");
#else
    png_file = std::fopen(png_path.c_str(), "wb");
#endif
    if (!png_file || !png_image_write_to_stdio(&png, png_file, 0, expected_pixels, 0, nullptr)) {
        if (png_file) {
            std::fclose(png_file);
        }
        std::cerr << "Could not create PNG test image\n";
        return 1;
    }
    std::fclose(png_file);
    const auto image = platform::read_gallery_image(png_path);
    if (!image.error.empty() || image.width != 2 || image.height != 1 ||
        image.pixels != std::vector<uint8_t>(std::begin(expected_pixels),
                                             std::end(expected_pixels))) {
        std::cerr << "Gallery must decode PNG pixels and transparency correctly\n";
        passed = false;
    }
    if (platform::read_gallery_image(temp / "sample.txt").error.empty() ||
        platform::read_gallery_image(temp / "missing.png").error.empty() ||
        platform::read_gallery_image(png_path, &cancelled).error.empty()) {
        std::cerr << "Gallery must report invalid, missing, and canceled image reads\n";
        passed = false;
    }
    const auto art_directory = temp / "art";
    if (!std::filesystem::create_directory(art_directory, error)) {
        std::cerr << "Could not create album art test directory: " << error.message() << '\n';
        return 1;
    }
    const auto track = art_directory / "track.MP3";
    std::ofstream(track) << "track";
    if (platform::read_album_art(track).error.empty() ||
        platform::read_album_art(track, &cancelled).error.empty() ||
        platform::read_album_art(temp / "sample.txt").error.empty()) {
        std::cerr << "Album art must report missing covers, cancellation and non-audio files\n";
        passed = false;
    }
    std::filesystem::copy_file(png_path, art_directory / "folder.png", error);
    if (error) {
        std::cerr << "Could not create album art fixture: " << error.message() << '\n';
        return 1;
    }
    const auto album = platform::read_album_art(track);
    if (!album.error.empty() || album.pixels != image.pixels) {
        std::cerr << "Album art must find and decode folder.png beside a track\n";
        passed = false;
    }
    std::ofstream(art_directory / "cover.jpg") << "invalid";
    if (platform::read_album_art(track).error.find("cover.jpg") == std::string::npos) {
        std::cerr << "Album art must report a corrupt higher-priority cover\n";
        passed = false;
    }
    const auto boundary_png = temp / "boundary.png";
    png.width = 1024;
    std::vector<uint8_t> boundary_pixels(static_cast<std::size_t>(png.width) * 4U, 255);
#if defined(_WIN32)
    png_file = _wfopen(boundary_png.c_str(), L"wb");
#else
    png_file = std::fopen(boundary_png.c_str(), "wb");
#endif
    if (!png_file || !png_image_write_to_stdio(&png, png_file, 0,
                                               boundary_pixels.data(), 0, nullptr)) {
        if (png_file) {
            std::fclose(png_file);
        }
        std::cerr << "Could not create boundary PNG test image\n";
        return 1;
    }
    std::fclose(png_file);
    if (!platform::read_gallery_image(boundary_png).error.empty()) {
        std::cerr << "Gallery must accept images at the 1024-pixel boundary\n";
        passed = false;
    }
    const auto oversized_png = temp / "oversized.png";
    png.width = 1025;
    boundary_pixels.resize(static_cast<std::size_t>(png.width) * 4U, 255);
#if defined(_WIN32)
    png_file = _wfopen(oversized_png.c_str(), L"wb");
#else
    png_file = std::fopen(oversized_png.c_str(), "wb");
#endif
    if (!png_file || !png_image_write_to_stdio(&png, png_file, 0,
                                               boundary_pixels.data(), 0, nullptr)) {
        if (png_file) {
            std::fclose(png_file);
        }
        std::cerr << "Could not create oversized PNG test image\n";
        return 1;
    }
    std::fclose(png_file);
    if (platform::read_gallery_image(oversized_png).error.empty()) {
        std::cerr << "Gallery must reject images wider than 1024 pixels\n";
        passed = false;
    }
    const auto large_file = temp / "large.png";
    constexpr std::streamoff file_limit = 8 * 1024 * 1024;
    {
        std::ofstream output(large_file, std::ios::binary);
        output.seekp(file_limit - 1);
        output.put('\0');
        if (!output) {
            std::cerr << "Could not create size-boundary test file\n";
            return 1;
        }
    }
    if (platform::read_gallery_image(large_file).error.find("8 MiB") != std::string::npos) {
        std::cerr << "Gallery must not reject files at the 8 MiB boundary for size\n";
        passed = false;
    }
    {
        std::ofstream output(large_file, std::ios::binary | std::ios::app);
        output.put('\0');
        if (!output) {
            std::cerr << "Could not create oversized test file\n";
            return 1;
        }
    }
    if (platform::read_gallery_image(large_file).error.find("8 MiB") == std::string::npos) {
        std::cerr << "Gallery must reject files larger than 8 MiB\n";
        passed = false;
    }
    const auto jpeg_path = temp / "image.jpg";
    const auto boundary_jpeg = temp / "boundary.jpg";
    const auto oversized_jpeg = temp / "wide.jpg";
    if (!write_jpeg(jpeg_path, 8) || !write_jpeg(boundary_jpeg, 1024) ||
        !write_jpeg(oversized_jpeg, 1025)) {
        std::cerr << "Could not create JPEG test images\n";
        return 1;
    }
    const auto jpeg = platform::read_gallery_image(jpeg_path);
    if (!jpeg.error.empty() || jpeg.width != 8 || jpeg.height != 1 ||
        jpeg.pixels.size() != 32 || jpeg.pixels[0] > 20 || jpeg.pixels[1] > 20 ||
        jpeg.pixels[2] < 230 || jpeg.pixels[3] != 255) {
        std::cerr << "Gallery must decode JPEG pixels as opaque BGRA\n";
        passed = false;
    }
    {
        std::ifstream input(jpeg_path, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                         std::istreambuf_iterator<char>());
        const auto first = platform::save_camera_jpeg(temp, bytes);
        const auto second = platform::save_camera_jpeg(temp, bytes);
        auto padded_bytes = bytes;
        padded_bytes.insert(padded_bytes.end(), {0, 0, 0});
        const auto padded = platform::save_camera_jpeg(temp, padded_bytes);
        std::ifstream saved(first.path, std::ios::binary);
        const std::vector<uint8_t> saved_bytes((std::istreambuf_iterator<char>(saved)),
                                               std::istreambuf_iterator<char>());
        std::ifstream saved_padded(padded.path, std::ios::binary);
        const std::vector<uint8_t> saved_padded_bytes(
            (std::istreambuf_iterator<char>(saved_padded)), std::istreambuf_iterator<char>());
        if (!first.error.empty() || !second.error.empty() || first.path == second.path ||
            !padded.error.empty() || saved_padded_bytes != padded_bytes ||
            !platform::read_gallery_image(padded.path).error.empty() ||
            saved_bytes != bytes || !platform::read_gallery_image(first.path).error.empty() ||
            platform::save_camera_jpeg(temp, {}).error.empty() ||
            platform::save_camera_jpeg(temp / "missing", bytes).error.empty()) {
            std::cerr << "Photo saving must preserve JPEG data, avoid overwrites, and report errors\n";
            passed = false;
        }
    }
    if (!platform::read_gallery_image(boundary_jpeg).error.empty() ||
        platform::read_gallery_image(oversized_jpeg).error.empty()) {
        std::cerr << "Gallery must enforce the 1024-pixel JPEG dimension boundary\n";
        passed = false;
    }
    {
        std::ofstream output(temp / "broken.jpg", std::ios::binary);
        output.put(static_cast<char>(0xff));
        output.put(static_cast<char>(0xd8));
        output.put('\0');
    }
    if (platform::read_gallery_image(temp / "broken.jpg").error.empty()) {
        std::cerr << "Gallery must report invalid JPEG files\n";
        passed = false;
    }
    {
        std::ifstream input(jpeg_path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        if (bytes.size() < 3) {
            std::cerr << "JPEG fixture is unexpectedly short\n";
            return 1;
        }
        std::ofstream output(temp / "truncated.jpg", std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - 2));
        if (!output) {
            std::cerr << "Could not create truncated JPEG fixture\n";
            return 1;
        }
    }
    if (platform::read_gallery_image(temp / "truncated.jpg").error.empty()) {
        std::cerr << "Gallery must report incomplete JPEG files\n";
        passed = false;
    }
    std::tm sample_time{};
    sample_time.tm_hour = 13;
    sample_time.tm_min = 5;
    sample_time.tm_sec = 9;
    passed &= expect_string(platform::format_clock_text(sample_time, true),
                            "13:05:09",
                            "24-hour clock");
    passed &= expect_string(platform::format_clock_text(sample_time, false),
                            "01:05:09 PM",
                            "12-hour clock");
    passed &= expect_string(platform::format_clock_text(sample_time, false, false),
                            "01:05 PM",
                            "12-hour title-bar clock");
    sample_time.tm_hour = 0;
    passed &= expect_string(platform::format_clock_text(sample_time, false),
                            "12:05:09 AM",
                            "12-hour midnight");
    sample_time.tm_hour = 12;
    passed &= expect_string(platform::format_clock_text(sample_time, false),
                            "12:05:09 PM",
                            "12-hour noon");
    passed &= expect_result("1+2*3", 7.0);
    passed &= expect_result("-(2 + 3) * .5", -2.5);
    passed &= expect_result("2--3", 5.0);
    passed &= expect_invalid("");
    passed &= expect_invalid("1..2");
    passed &= expect_invalid("1/(2-2)");
    passed &= expect_invalid(std::string(65, '1'));
    passed &= expect_string(platform::encode_morse("SOS"),
                            "... --- ...",
                            "Morse encoding");
    passed &= expect_string(platform::decode_morse("... --- ..."),
                            "SOS",
                            "Morse decoding");
    return passed ? 0 : 1;
}
