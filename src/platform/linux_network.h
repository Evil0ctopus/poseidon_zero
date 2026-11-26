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
bool is_ethernet_interface_type(int type, bool wireless);
std::vector<WifiNetwork> scan_wifi_networks();
std::string ping_host(const std::string& host);
struct DnsLookupResult {
    std::string output;
    std::string error;
};
DnsLookupResult dns_lookup(const std::string& host);
bool valid_https_host(const std::string& host);
struct HttpsCheckResult {
    bool verified{false};
    std::string detail;
};
HttpsCheckResult check_https_certificate(const std::string& host);
struct InternetCheckResult {
    bool connected{false};
    std::string detail;
};
InternetCheckResult check_internet();
std::vector<std::string> scan_bluetooth_devices();
std::string default_gateway();
std::string parse_default_gateway(std::istream& routes);
ArpResult parse_arp_table(std::istream& arp);
ArpResult arp_entries();

} // namespace platform
