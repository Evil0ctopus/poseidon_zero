#pragma once

#include <string>
#include <vector>

namespace platform {

struct NetworkInterface {
    std::string name;
    std::string address;
    bool connected{false};
};

struct WifiNetwork {
    std::string ssid;
    std::string security;
    int signal{-1};
};

std::vector<NetworkInterface> list_network_interfaces();
std::vector<WifiNetwork> scan_wifi_networks();
std::string ping_host(const std::string& host);
std::string dns_lookup(const std::string& host);
std::vector<std::string> scan_bluetooth_devices();
std::string default_gateway();
std::vector<std::string> arp_entries();

} // namespace platform
