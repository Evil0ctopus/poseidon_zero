#include "network_inventory.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main() {
    using namespace platform;
    const ArpEntry selected{"192.168.7.1", "12:34:56:78:9a:bc", "eth0"};
    ArpResult old_arp;
    old_arp.entries = {selected};
    ArpResult new_arp;
    new_arp.entries = {{"192.168.7.1", "12:34:56:78:9A:BC", "eth0"}};
    if (compare_arp_entry(old_arp, selected, new_arp).state != ArpComparisonState::Same) {
        std::cerr << "Unchanged ARP entry was not recognized\n";
        return 1;
    }
    new_arp.entries[0].hardware_address = "12:34:56:78:9a:bd";
    const auto changed_arp = compare_arp_entry(old_arp, selected, new_arp);
    if (changed_arp.state != ArpComparisonState::Changed ||
        changed_arp.current_mac != "12:34:56:78:9a:bd") {
        std::cerr << "Changed ARP MAC was not recognized\n";
        return 1;
    }
    new_arp.entries.clear();
    if (compare_arp_entry(old_arp, selected, new_arp).state != ArpComparisonState::Unknown) {
        std::cerr << "Missing ARP entry should be unknown\n";
        return 1;
    }
    new_arp.entries = {selected, selected};
    if (compare_arp_entry(old_arp, selected, new_arp).state != ArpComparisonState::Unknown) {
        std::cerr << "Ambiguous ARP entries should be unknown\n";
        return 1;
    }
    new_arp.entries = {selected};
    new_arp.truncated = true;
    if (compare_arp_entry(old_arp, selected, new_arp).state != ArpComparisonState::Unknown) {
        std::cerr << "Truncated ARP snapshot should be unknown\n";
        return 1;
    }
    new_arp.truncated = false;
    new_arp.entries[0].interface_name = "wlan0";
    if (compare_arp_entry(old_arp, selected, new_arp).state != ArpComparisonState::Unknown) {
        std::cerr << "Different interface should not be compared\n";
        return 1;
    }
    new_arp.error = "read failed";
    if (compare_arp_entry(old_arp, selected, new_arp).detail != "read failed") {
        std::cerr << "ARP read error was not surfaced\n";
        return 1;
    }
    new_arp.error.clear();
    new_arp.entries = {{"192.168.7.1", "00:00:00:00:00:00", "eth0"}};
    if (compare_arp_entry(old_arp, selected, new_arp).state != ArpComparisonState::Unknown) {
        std::cerr << "Unusable ARP address should be unknown\n";
        return 1;
    }
    if (!valid_inventory_date("2024-02-29") || valid_inventory_date("2025-02-29") ||
        valid_inventory_date("2024-13-01") || valid_inventory_date("2024-2-01") ||
        valid_inventory_date("0000-01-01")) {
        std::cerr << "Reminder date validation failed\n";
        return 1;
    }
    const auto targets = numeric_port_targets("192.168.7.0/24");
    if (!targets.error.empty() || targets.addresses.size() != 254 ||
        numeric_port_targets("192.168.0.0/23").error.empty() ||
        numeric_port_targets("example.com").error.empty()) {
        std::cerr << "Numeric target validation failed\n";
        return 1;
    }
    const auto excluded = exclude_inventory_targets(targets, "192.168.7.1,192.168.7.3");
    if (!excluded.error.empty() || excluded.addresses.size() != 252 ||
        exclude_inventory_targets(targets, "192.168.7.1,192.168.7.1").error.empty() ||
        exclude_inventory_targets(targets, "example.com").error.empty()) {
        std::cerr << "Inventory exclusions failed\n";
        return 1;
    }

    PortScanResult scan;
    scan.targets = {"192.168.7.1", "192.168.7.2", "192.168.7.3", "192.168.7.4"};
    scan.ports = {22, 80};
    scan.total = 8;
    scan.completed = 6;
    scan.cancelled = true;
    scan.entries = {
        {"192.168.7.1", 22, PortProtocol::Tcp, PortState::Closed},
        {"192.168.7.1", 80, PortProtocol::Tcp, PortState::Open},
        {"192.168.7.2", 22, PortProtocol::Tcp, PortState::Filtered},
        {"192.168.7.3", 22, PortProtocol::Tcp, PortState::Closed},
        {"192.168.7.4", 22, PortProtocol::Tcp, PortState::Closed},
    };
    ArpResult arp;
    arp.truncated = true;
    arp.entries = {
        {"192.168.7.1", "12:34:56:78:9a:bc", "eth0"},
        {"192.168.7.3", "12:34:56:78:9a:bd", "eth0"},
        {"192.168.7.3", "12:34:56:78:9a:be", "wlan0"},
        {"192.168.7.4", "00:00:00:00:00:00", "eth0"},
    };
    auto inventory = make_network_inventory(scan, arp);
    if (inventory.hosts.size() != 3 || inventory.hosts[0].address != "192.168.7.1" ||
        inventory.hosts[0].cached_mac != "12:34:56:78:9a:bc" ||
        inventory.hosts[0].open_ports != std::vector<uint16_t>{80} ||
        !inventory.hosts[1].cached_mac.empty() || !inventory.hosts[2].cached_mac.empty() ||
        inventory.arp_warning.empty()) {
        std::cerr << "Inventory response or ARP classification failed\n";
        return 1;
    }
    const auto policy = review_inventory_policy(inventory, inventory.hosts[0], {22, 23, 80, 443});
    const auto no_evidence = review_inventory_policy(inventory, inventory.hosts[1], {22, 23});
    const auto guidance = inventory_port_guidance(inventory.hosts[0]);
    const auto no_guidance = inventory_port_guidance(inventory.hosts[1]);
    if (policy.observed_open != std::vector<uint16_t>{80} ||
        policy.not_in_scan != std::vector<uint16_t>({23, 443}) ||
        !no_evidence.observed_open.empty() ||
        no_evidence.not_in_scan != std::vector<uint16_t>{23} ||
        guidance.size() != 1 || guidance[0].find("TCP 80") == std::string::npos ||
        !no_guidance.empty()) {
        std::cerr << "TCP exposure policy review failed\n";
        return 1;
    }
    const auto inspected = parse_nmap_grepable(
        "Host: 192.168.7.1 ()\tPorts: 22/open/tcp//ssh//OpenSSH 9.0, protocol 2/, "
        "80/closed/tcp//http///\tOS: Linux 5.x\n",
        "192.168.7.1", {22, 80});
    if (!inspected.error.empty() || inspected.services.size() != 1 ||
        inspected.services[0].version != "OpenSSH 9.0, protocol 2" || inspected.os_guess != "Linux 5.x" ||
        parse_nmap_grepable("Host: 192.168.7.1 ()\tPorts: 999/open/tcp//x//v/\n",
                            "192.168.7.1", {22}).error.empty() ||
        parse_nmap_grepable("Host: 192.168.7.2 ()\tStatus: Up\n",
                            "192.168.7.1", {22}).error.empty()) {
        std::cerr << "Nmap output classification failed\n";
        return 1;
    }
    inventory.hosts[0].services = inspected.services;
    inventory.hosts[0].os_guess = inspected.os_guess;
    inventory.hosts[0].label = "Office computer";
    inventory.hosts[0].update_note = "Check updates";
    inventory.hosts[0].update_due = "2028-02-29";
    const std::string router_csv =
        "ip,mac,hostname\n192.168.7.1,aa:bb:cc:dd:ee:ff,\"Office, desktop\"\n"
        "192.168.7.2,,Silent client\n10.1.0.4,,Other network\n";
    const auto clients = parse_router_clients(router_csv);
    const auto imported = add_router_evidence(inventory, clients);
    if (!clients.error.empty() || clients.entries.size() != 3 ||
        !imported.error.empty() || imported.matched != 1 || imported.imported_only != 2 ||
        inventory.hosts.size() != 5 || inventory.hosts[3].responded ||
        !inventory.hosts[3].router_listed || inventory.hosts[4].responded ||
        inventory.hosts[0].router_name != "Office, desktop" ||
        inventory.hosts[0].router_mac != "aa:bb:cc:dd:ee:ff" ||
        !inventory_mac_conflict(inventory.hosts[0]) ||
        parse_router_clients("ip,mac,hostname\n192.168.7.1,,a\n192.168.7.1,,b\n").error.empty() ||
        parse_router_clients("ip,mac,hostname\n192.168.7.1,,\"unterminated\n").error.empty()) {
        std::cerr << "Router import failed\n";
        return 1;
    }
    auto reimported = inventory;
    reimported.hosts[3].label = "Keep annotation";
    const auto repeated = add_router_evidence(reimported, clients);
    if (!repeated.error.empty() || repeated.imported_only != 2 ||
        reimported.hosts.size() != 5 || reimported.hosts[3].label != "Keep annotation") {
        std::cerr << "Repeated import must preserve matching annotations\n";
        return 1;
    }
    const auto replaced = add_router_evidence(reimported, parse_router_clients("ip,mac,hostname\n"));
    if (!replaced.error.empty() || replaced.retained_without_record != 1 ||
        reimported.hosts.size() != 4 || reimported.hosts[3].router_listed ||
        reimported.hosts[3].label != "Keep annotation") {
        std::cerr << "Router reimport must preserve annotations without stale records\n";
        return 1;
    }
    NetworkInventory at_limit;
    for (int i = 0; i < 512; ++i) {
        InventoryHost host;
        host.address = "198.51." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        host.responded = false;
        host.label = "Keep";
        at_limit.hosts.push_back(std::move(host));
    }
    at_limit.hosts[0].router_listed = true;
    at_limit.hosts[0].router_name = "Existing";
    const auto overflow = add_router_evidence(
        at_limit, parse_router_clients("ip,mac,hostname\n203.0.113.1,,New\n"));
    if (overflow.error.empty() || at_limit.hosts.size() != 512 ||
        at_limit.hosts[0].label != "Keep" || !at_limit.hosts[0].router_listed ||
        at_limit.hosts[0].router_name != "Existing") {
        std::cerr << "An over-limit import must leave the inventory unchanged\n";
        return 1;
    }
    std::string full_csv = "ip,mac,hostname\n";
    for (int i = 0; i < 256; ++i)
        full_csv += "198.51.100." + std::to_string(i) + ",,\n";
    if (parse_router_clients(full_csv).entries.size() != 256 ||
        parse_router_clients(full_csv + "203.0.113.1,,\n").error.empty()) {
        std::cerr << "Router import row limit failed\n";
        return 1;
    }
    inventory.comparison_note = "Partial: new responses 1";
    inventory.comparison_baseline = "poseidon-host-discovery-123-0.txt";
    const auto directory = std::filesystem::temp_directory_path() /
        ("poseidon-inventory-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto report = save_network_inventory(directory, inventory);
    if (!report.error.empty()) {
        std::cerr << report.error << '\n';
        return 1;
    }
    std::ifstream input(report.path);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    const auto history = list_network_inventories(directory);
    const auto name = report.path.filename().u8string();
    const auto loaded = load_network_inventory(directory, name);
    const std::string early_name = "poseidon-host-discovery--123-0.txt";
    std::filesystem::copy_file(report.path, directory / early_name);
    if (list_network_inventories(directory).names.size() != 2 ||
        !load_network_inventory(directory, early_name).scan.error.empty() ||
        !save_inventory_baseline(directory, early_name).empty() ||
        load_inventory_baseline(directory).name != early_name ||
        !delete_network_inventory(directory, early_name).empty() ||
        !clear_inventory_baseline(directory, early_name).empty() ||
        !load_inventory_baseline(directory).name.empty() ||
        delete_network_inventory(directory, "poseidon-host-discovery---123-0.txt").empty()) {
        std::cerr << "Pre-1970 inventory management failed\n";
        return 1;
    }
    if (!load_inventory_baseline(directory).name.empty() ||
        !save_inventory_baseline(directory, name).empty() ||
        load_inventory_baseline(directory).name != name ||
        !save_inventory_baseline(directory, name).empty() ||
        save_inventory_baseline(directory, "../outside").empty() ||
        !clear_inventory_baseline(directory, "poseidon-host-discovery-1-0.txt").empty() ||
        load_inventory_baseline(directory).name != name) {
        std::cerr << "Persistent baseline selection failed\n";
        return 1;
    }
    {
        std::ofstream corrupt(directory / "inventory-baseline", std::ios::trunc);
        corrupt << "../outside";
    }
    if (load_inventory_baseline(directory).error.empty() ||
        !save_inventory_baseline(directory, name).empty() ||
        load_inventory_baseline(directory).name != name) {
        std::cerr << "Corrupt baseline selection was not rejected or replaced\n";
        return 1;
    }
#if !defined(_WIN32)
    {
        std::error_code error;
        std::filesystem::remove(directory / "inventory-baseline", error);
        if (error) return 1;
        std::filesystem::create_symlink(report.path, directory / "inventory-baseline", error);
        if (error || load_inventory_baseline(directory).error.empty() ||
            !save_inventory_baseline(directory, name).empty() ||
            load_inventory_baseline(directory).name != name ||
            !load_network_inventory(directory, name).scan.error.empty()) {
            std::cerr << "Baseline symlink must be rejected and replaced safely\n";
            return 1;
        }
    }
#endif
    if (!history.error.empty() || history.names != std::vector<std::string>{name} ||
        !loaded.scan.error.empty() || !loaded.scan.cancelled ||
        loaded.scan.targets != scan.targets || loaded.scan.ports != scan.ports ||
        loaded.hosts.size() != inventory.hosts.size() ||
        loaded.hosts[0].cached_mac != inventory.hosts[0].cached_mac ||
        loaded.hosts[0].services.size() != 1 ||
        loaded.hosts[0].services[0].version != "OpenSSH 9.0, protocol 2" ||
        loaded.hosts[0].os_guess != "Linux 5.x" ||
        loaded.arp_warning != "ARP cache truncated" ||
        loaded.comparison_note != inventory.comparison_note ||
        loaded.comparison_baseline != inventory.comparison_baseline ||
        loaded.hosts[0].label != "Office computer" ||
        loaded.hosts[0].update_note != "Check updates" ||
        loaded.hosts[0].update_due != "2028-02-29" ||
        loaded.hosts[0].router_name != "Office, desktop" ||
        loaded.hosts[0].router_mac != "aa:bb:cc:dd:ee:ff" ||
        loaded.hosts[3].responded || !loaded.hosts[3].router_listed ||
        loaded.hosts[4].responded || !loaded.hosts[4].router_listed ||
        load_network_inventory(directory, "../outside.txt").scan.error.empty() ||
        delete_network_inventory(directory, "../outside.txt").empty()) {
        std::cerr << "Inventory history failed\n";
        return 1;
    }
    const auto retained_report = save_network_inventory(directory, reimported);
    const auto retained_loaded = load_network_inventory(directory, retained_report.path.filename().u8string());
    if (!retained_report.error.empty() || !retained_loaded.scan.error.empty() ||
        retained_loaded.hosts.size() != 4 || retained_loaded.hosts[3].responded ||
        retained_loaded.hosts[3].router_listed ||
        retained_loaded.hosts[3].label != "Keep annotation" ||
        !delete_network_inventory(directory, retained_report.path.filename().u8string()).empty()) {
        std::cerr << "Retained annotations failed to round-trip\n";
        return 1;
    }
    const std::string legacy_name = "poseidon-host-discovery-3-0.txt";
    {
        std::ofstream legacy(directory / legacy_name);
        legacy << "@inventory 1 1 1 0 0 0\n@ports 22\n@target 192.0.2.1\n"
                  "@host 192.0.2.1 - - -\n@router 192.0.2.1 \"Old\" \"\"\n";
    }
    const auto legacy = load_network_inventory(directory, legacy_name);
    if (!legacy.scan.error.empty() || legacy.hosts.size() != 1 ||
        !legacy.hosts[0].responded || !legacy.hosts[0].router_listed ||
        !delete_network_inventory(directory, legacy_name).empty()) {
        std::cerr << "Legacy report compatibility failed\n";
        return 1;
    }
    {
        std::ofstream source(directory / "router-clients.csv");
        source << router_csv;
    }
    if (!load_router_clients(directory).error.empty() ||
        load_router_clients(directory).entries.size() != 3) {
        std::cerr << "Router export file failed\n";
        return 1;
    }
    std::error_code router_error;
    std::filesystem::remove(directory / "router-clients.csv", router_error);
    if (router_error) return 1;
#if !defined(_WIN32)
    std::filesystem::create_symlink(report.path, directory / "router-clients.csv", router_error);
    if (router_error || load_router_clients(directory).error.empty()) {
        std::cerr << "Router export symlink must be rejected\n";
        return 1;
    }
    std::filesystem::remove(directory / "router-clients.csv", router_error);
    if (router_error) return 1;
#endif
    auto later = loaded;
    later.hosts[0].cached_mac = "12:34:56:78:9A:BC";
    if (compare_network_inventories(loaded, later).mac_changed != 0) {
        std::cerr << "MAC case should not indicate a change\n";
        return 1;
    }
    auto no_response = loaded;
    no_response.hosts[0].responded = false;
    no_response.hosts[0].open_ports.clear();
    const auto missing = compare_network_inventories(loaded, no_response);
    if (missing.not_seen != 1 || missing.newly_responding != 0) {
        std::cerr << "Router-only records must not count as scan responses\n";
        return 1;
    }
    later.hosts.erase(later.hosts.begin() + 1);
    later.hosts[0].cached_mac = "12:34:56:78:9a:bf";
    later.hosts[0].open_ports.push_back(22);
    later.hosts[0].services[0].version = "OpenSSH 9.1";
    for (auto& host : later.hosts) {
        if (host.address == "192.168.7.2") {
            host.responded = true;
            host.open_ports = {80};
        }
    }
    const auto comparison = compare_network_inventories(loaded, later);
    if (!comparison.comparable || comparison.newly_responding != 1 ||
        comparison.not_seen != 1 || comparison.mac_changed != 1 ||
        comparison.newly_open_ports != 2 || comparison.service_changes != 1 ||
        comparison.details.size() != 6) {
        std::cerr << "Inventory comparison failed\n";
        return 1;
    }
    later.scan.targets.pop_back();
    if (compare_network_inventories(loaded, later).comparable) {
        std::cerr << "Different scan coverage compared as equal\n";
        return 1;
    }
    const auto invalid_name = std::string("poseidon-host-discovery-1-0.txt");
    {
        std::ofstream invalid(directory / invalid_name);
        invalid << "@inventory 1 1 1 0 0 0\n@ports 22\n@target 192.0.2.1\n"
                   "@host 192.0.2.2 - - -\n";
    }
    if (load_network_inventory(directory, invalid_name).scan.error.empty() ||
        !delete_network_inventory(directory, invalid_name).empty()) {
        std::cerr << "Malformed report must be rejected\n";
        return 1;
    }
#if !defined(_WIN32)
    const auto link_name = std::string("poseidon-host-discovery-2-0.txt");
    std::error_code link_error;
    std::filesystem::create_symlink(report.path, directory / link_name, link_error);
    if (link_error || load_network_inventory(directory, link_name).scan.error.empty() ||
        delete_network_inventory(directory, link_name).empty()) {
        std::cerr << "Report symlink must be rejected\n";
        return 1;
    }
    std::filesystem::remove(directory / link_name, link_error);
    if (link_error) return 1;
#endif
    const auto deletion = delete_network_inventory(directory, name);
    if (load_inventory_baseline(directory).error.empty() ||
        !clear_inventory_baseline(directory, name).empty() ||
        !load_inventory_baseline(directory).error.empty() ||
        !load_inventory_baseline(directory).name.empty()) {
        std::cerr << "Missing baseline report or cleanup failed\n";
        return 1;
    }
    if (!deletion.empty() || !list_network_inventories(directory).names.empty()) {
        std::cerr << "Inventory deletion failed\n";
        return 1;
    }
    for (int i = 0; i < 257; ++i) {
        std::ofstream fixture(directory / ("poseidon-host-discovery-5-" + std::to_string(i) + ".txt"));
        if (!fixture) return 1;
    }
    const auto full = list_network_inventories(directory);
    if (!full.error.empty() || !full.truncated || full.names.size() != 256 ||
        save_network_inventory(directory, inventory).error.empty()) {
        std::cerr << "Report limit must remain browsable and prevent new saves\n";
        return 1;
    }
    for (int i = 0; i < 257; ++i) {
        if (!delete_network_inventory(directory,
                "poseidon-host-discovery-5-" + std::to_string(i) + ".txt").empty()) return 1;
    }
    std::error_code error;
    std::filesystem::remove(directory, error);
    if (error || text.find("Cancelled (partial results)") == std::string::npos ||
        text.find("Unseen targets are unknown") == std::string::npos ||
        text.find("Responding addresses: 3") == std::string::npos ||
        text.find("10.1.0.4 (not observed responding)") == std::string::npos ||
        text.find("192.168.7.1  cached MAC: 12:34:56:78:9a:bc") == std::string::npos ||
        text.find("ARP cache truncated") == std::string::npos) {
        std::cerr << "Inventory report failed\n";
        return 1;
    }
    return 0;
}
