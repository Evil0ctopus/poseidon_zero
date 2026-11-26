#pragma once

#include "linux_network.h"
#include "nmap_scan.h"
#include "port_scan.h"

namespace platform {

struct InventoryHost {
    std::string address;
    std::string cached_mac;
    std::string interface_name;
    std::vector<uint16_t> open_ports;
    std::vector<NmapService> services;
    std::string os_guess;
    std::string label;
    std::string update_note;
    std::string update_due;
    std::string router_name;
    std::string router_mac;
    bool responded{true};
    bool router_listed{false};
};

struct NetworkInventory {
    PortScanResult scan;
    std::vector<InventoryHost> hosts;
    std::string arp_warning;
    std::string comparison_note;
    std::string comparison_baseline;
};

struct InventoryHistory {
    std::vector<std::string> names;
    std::string error;
    bool truncated{false};
};

struct InventoryBaseline {
    std::string name;
    NetworkInventory inventory;
    std::string error;
};

struct InventoryComparison {
    bool comparable{false};
    std::size_t newly_responding{0};
    std::size_t not_seen{0};
    std::size_t mac_changed{0};
    std::size_t newly_open_ports{0};
    std::size_t service_changes{0};
    std::vector<std::string> details;
    bool details_truncated{false};
};

struct InventoryPolicyReview {
    std::vector<uint16_t> observed_open;
    std::vector<uint16_t> not_in_scan;
};

enum class ArpComparisonState { Same, Changed, Unknown };
struct ArpComparison {
    ArpComparisonState state{ArpComparisonState::Unknown};
    std::string current_mac;
    std::string detail;
};

struct RouterClient {
    std::string address;
    std::string mac;
    std::string name;
};

struct RouterClients {
    std::vector<RouterClient> entries;
    std::string error;
};

struct RouterImportResult {
    std::size_t matched{0};
    std::size_t imported_only{0};
    std::size_t retained_without_record{0};
    std::string error;
};

NetworkInventory make_network_inventory(PortScanResult scan, const ArpResult& arp);
PortReportResult save_network_inventory(const std::filesystem::path& directory,
                                        const NetworkInventory& inventory);
InventoryHistory list_network_inventories(const std::filesystem::path& directory);
NetworkInventory load_network_inventory(const std::filesystem::path& directory,
                                        const std::string& name);
std::string delete_network_inventory(const std::filesystem::path& directory,
                                     const std::string& name);
InventoryBaseline load_inventory_baseline(const std::filesystem::path& directory,
                                          bool load_report = true);
std::string save_inventory_baseline(const std::filesystem::path& directory,
                                    const std::string& name);
std::string clear_inventory_baseline(const std::filesystem::path& directory,
                                     const std::string& deleted_name);
InventoryComparison compare_network_inventories(const NetworkInventory& baseline,
                                                const NetworkInventory& current);
InventoryPolicyReview review_inventory_policy(const NetworkInventory& inventory,
                                               const InventoryHost& host,
                                               const std::vector<uint16_t>& restricted_ports);
std::vector<std::string> inventory_port_guidance(const InventoryHost& host);
PortTargets exclude_inventory_targets(const PortTargets& targets, const std::string& exclusions);
bool valid_inventory_date(const std::string& date);
std::string inventory_local_date();
RouterClients parse_router_clients(const std::string& csv);
RouterClients load_router_clients(const std::filesystem::path& directory);
RouterImportResult add_router_evidence(NetworkInventory& inventory, const RouterClients& clients);
bool inventory_mac_conflict(const InventoryHost& host);
ArpComparison compare_arp_entry(const ArpResult& baseline, const ArpEntry& selected,
                                const ArpResult& current);

} // namespace platform
