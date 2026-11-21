#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace platform {

struct NetworkInterface {
    std::string name;
    std::string address;
    bool connected{false};
    bool state_known{true};
    bool ethernet{false};
};

struct NetworkInterfacesResult {
    std::vector<NetworkInterface> interfaces;
    std::string error;
};

struct WifiNetwork {
    std::string ssid;
    std::string security;
    int signal{-1};
};

struct ArpEntry {
    std::string address;
    std::string hardware_address;
    std::string interface_name;
};

struct ArpResult {
    std::vector<ArpEntry> entries;
    std::string error;
    bool truncated{false};
};

NetworkInterfacesResult list_network_interfaces();
NetworkInterfacesResult filter_ethernet_interfaces(NetworkInterfacesResult interfaces);
std::vector<WifiNetwork> scan_wifi_networks();
std::string ping_host(const std::string& host);
std::string dns_lookup(const std::string& host);
std::vector<std::string> scan_bluetooth_devices();
std::string default_gateway();
std::string parse_default_gateway(std::istream& routes);
ArpResult parse_arp_table(std::istream& arp);
ArpResult arp_entries();

} // namespace platform
