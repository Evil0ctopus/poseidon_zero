#include "linux_network.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

#if USE_DESKTOP && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

namespace platform {

NetworkInterfacesResult filter_ethernet_interfaces(NetworkInterfacesResult interfaces) {
    auto& entries = interfaces.interfaces;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const NetworkInterface& item) { return !item.ethernet; }),
                  entries.end());
    return interfaces;
}

std::string parse_default_gateway(std::istream& routes) {
    std::string line;
    if (!std::getline(routes, line) || line.find("Destination") == std::string::npos) {
        return "Route table unavailable";
    }

    uint32_t best_gateway = 0;
    uint32_t best_metric = std::numeric_limits<uint32_t>::max();
    bool found = false;
    bool invalid = false;
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string iface, destination, gateway, flags, ref_count, use, metric, mask;
        if (!(fields >> iface >> destination >> gateway >> flags >> ref_count >> use >> metric >> mask)) {
            invalid = true;
            continue;
        }
        uint32_t destination_value = 0;
        const auto dest_result = std::from_chars(destination.data(),
                                                 destination.data() + destination.size(),
                                                 destination_value, 16);
        if (dest_result.ec != std::errc{} ||
            dest_result.ptr != destination.data() + destination.size()) {
            invalid = true;
            continue;
        }
        if (destination_value != 0) {
            continue;
        }
        uint32_t gateway_value = 0;
        uint32_t flags_value = 0;
        uint32_t metric_value = 0;
        uint32_t mask_value = 0;
        const auto gateway_result = std::from_chars(gateway.data(), gateway.data() + gateway.size(),
                                                    gateway_value, 16);
        const auto flags_result = std::from_chars(flags.data(), flags.data() + flags.size(),
                                                  flags_value, 16);
        const auto metric_result = std::from_chars(metric.data(), metric.data() + metric.size(),
                                                   metric_value, 10);
        const auto mask_result = std::from_chars(mask.data(), mask.data() + mask.size(),
                                                 mask_value, 16);
        if (gateway_result.ec != std::errc{} || gateway_result.ptr != gateway.data() + gateway.size() ||
            flags_result.ec != std::errc{} || flags_result.ptr != flags.data() + flags.size() ||
            metric_result.ec != std::errc{} || metric_result.ptr != metric.data() + metric.size() ||
            mask_result.ec != std::errc{} || mask_result.ptr != mask.data() + mask.size()) {
            invalid = true;
            continue;
        }
        if (mask_value != 0 || (flags_value & 0x1U) == 0) {
            continue;
        }
        if (!found || metric_value < best_metric) {
            best_gateway = (flags_value & 0x2U) != 0 ? gateway_value : 0;
            best_metric = metric_value;
            found = true;
        }
    }
    if (routes.bad()) {
        return "Route table read failed";
    }
    if (!found) {
        return invalid ? "Gateway data invalid" : "No gateway";
    }
    if (best_gateway == 0) {
        return "No gateway";
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u",
                  static_cast<unsigned>(best_gateway & 0xffU),
                  static_cast<unsigned>((best_gateway >> 8) & 0xffU),
                  static_cast<unsigned>((best_gateway >> 16) & 0xffU),
                  static_cast<unsigned>((best_gateway >> 24) & 0xffU));
    return text;
}

ArpResult parse_arp_table(std::istream& arp) {
    ArpResult result;
    std::string line;
    if (!std::getline(arp, line) || line.find("IP address") == std::string::npos) {
        result.error = "ARP table unavailable";
        return result;
    }
    while (std::getline(arp, line)) {
        std::istringstream fields(line);
        ArpEntry entry;
        std::string type, flags, mask;
        if (!(fields >> entry.address >> type >> flags >> entry.hardware_address >>
              mask >> entry.interface_name)) {
            result.entries.clear();
            result.error = "ARP table data invalid";
            return result;
        }
        if (result.entries.size() == 32) {
            result.truncated = true;
            break;
        }
        result.entries.push_back(std::move(entry));
    }
    if (arp.bad()) {
        result.entries.clear();
        result.truncated = false;
        result.error = "ARP table read failed";
    }
    return result;
}

} // namespace platform

#if USE_DESKTOP
namespace platform {
NetworkInterfacesResult list_network_interfaces() {
    NetworkInterfacesResult result;
#if defined(_WIN32)
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER;
    ULONG bytes = 0;
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bytes);
    if (status == ERROR_NO_DATA) {
        return result;
    }
    if (status != ERROR_BUFFER_OVERFLOW) {
        result.error = "Cannot read host adapters (error " + std::to_string(status) + ")";
        return result;
    }
    std::vector<IP_ADAPTER_ADDRESSES> storage;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (bytes > 1024U * 1024U) {
            result.error = "Host adapter data exceeds 1 MiB";
            return result;
        }
        storage.resize((bytes + sizeof(IP_ADAPTER_ADDRESSES) - 1) /
                       sizeof(IP_ADAPTER_ADDRESSES));
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, storage.data(), &bytes);
        if (status != ERROR_BUFFER_OVERFLOW) {
            break;
        }
    }
    if (status == ERROR_NO_DATA) {
        return result;
    }
    if (status != NO_ERROR) {
        result.error = "Cannot read host adapters (error " + std::to_string(status) + ")";
        return result;
    }
    for (const auto* adapter = storage.data(); adapter; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        NetworkInterface network_interface;
        if (!adapter->FriendlyName) {
            result.error = "Host adapter name unavailable";
            result.interfaces.clear();
            return result;
        }
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                               adapter->FriendlyName, -1,
                                               nullptr, 0, nullptr, nullptr);
        if (length <= 0) {
            result.error = "Cannot decode host adapter name";
            result.interfaces.clear();
            return result;
        }
        network_interface.name.resize(static_cast<std::size_t>(length));
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, adapter->FriendlyName, -1,
                                &network_interface.name[0], length, nullptr, nullptr) == 0) {
            result.error = "Cannot decode host adapter name";
            result.interfaces.clear();
            return result;
        }
        network_interface.name.pop_back();
        network_interface.state_known = adapter->OperStatus != IfOperStatusUnknown;
        network_interface.connected = adapter->OperStatus == IfOperStatusUp;
        network_interface.ethernet = adapter->IfType == IF_TYPE_ETHERNET_CSMACD;
        for (const auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (address->Address.lpSockaddr &&
                address->Address.lpSockaddr->sa_family == AF_INET) {
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
                const auto* octets = reinterpret_cast<const unsigned char*>(&ipv4->sin_addr);
                char text[16]{};
                std::snprintf(text, sizeof(text), "%u.%u.%u.%u",
                              static_cast<unsigned>(octets[0]), static_cast<unsigned>(octets[1]),
                              static_cast<unsigned>(octets[2]), static_cast<unsigned>(octets[3]));
                network_interface.address = text;
                break;
            }
        }
        result.interfaces.push_back(std::move(network_interface));
    }
#else
    result.error = "Host adapter status unsupported on this desktop";
#endif
    return result;
}
std::vector<WifiNetwork> scan_wifi_networks() {
    return {{"Emulator Wi-Fi", "WPA2", 75}};
}
std::string ping_host(const std::string&) { return "Desktop emulator: ping disabled"; }
std::string dns_lookup(const std::string&) { return "Desktop emulator: DNS disabled"; }
std::vector<std::string> scan_bluetooth_devices() { return {"Desktop emulator: no BLE data"}; }
std::string default_gateway() {
#if defined(_WIN32)
    MIB_IPFORWARD_TABLE2* raw_table = nullptr;
    const auto status = GetIpForwardTable2(AF_INET, &raw_table);
    if (status != NO_ERROR) {
        return "Route table unavailable (error " + std::to_string(status) + ")";
    }
    const std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(&FreeMibTable)> table(
        raw_table, &FreeMibTable);
    if (!table) {
        return "Route table unavailable";
    }

    uint64_t best_metric = std::numeric_limits<uint64_t>::max();
    uint32_t best_gateway = 0;
    bool found = false;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const auto& route = table->Table[index];
        if (route.DestinationPrefix.PrefixLength != 0 ||
            route.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr != 0 ||
            route.NextHop.si_family != AF_INET || route.Loopback ||
            (!route.Immortal && route.ValidLifetime == 0)) {
            continue;
        }
        MIB_IPINTERFACE_ROW ip_interface{};
        ip_interface.Family = AF_INET;
        ip_interface.InterfaceLuid = route.InterfaceLuid;
        const auto interface_status = GetIpInterfaceEntry(&ip_interface);
        if (interface_status != NO_ERROR) {
            return "Route table unavailable (error " + std::to_string(interface_status) + ")";
        }
        if (!ip_interface.Connected || ip_interface.DisableDefaultRoutes) {
            continue;
        }
        const uint64_t metric = static_cast<uint64_t>(route.Metric) + ip_interface.Metric;
        if (!found || metric < best_metric) {
            found = true;
            best_metric = metric;
            best_gateway = route.NextHop.Ipv4.sin_addr.s_addr;
        }
    }
    if (!found || best_gateway == 0) {
        return "No gateway";
    }
    const auto* octets = reinterpret_cast<const unsigned char*>(&best_gateway);
    char text[16]{};
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u",
                  static_cast<unsigned>(octets[0]), static_cast<unsigned>(octets[1]),
                  static_cast<unsigned>(octets[2]), static_cast<unsigned>(octets[3]));
    return text;
#else
    return "Route table unavailable";
#endif
}
ArpResult arp_entries() {
    ArpResult result;
#if defined(_WIN32)
    ULONG bytes = 0;
    DWORD status = GetIpNetTable(nullptr, &bytes, FALSE);
    if (status == ERROR_NO_DATA) {
        return result;
    }
    if (status != ERROR_INSUFFICIENT_BUFFER) {
        result.error = "Cannot read host ARP table (error " + std::to_string(status) + ")";
        return result;
    }
    std::vector<MIB_IPNETTABLE> storage;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (bytes > 1024U * 1024U) {
            result.error = "Host ARP data exceeds 1 MiB";
            return result;
        }
        storage.resize((bytes + sizeof(MIB_IPNETTABLE) - 1) / sizeof(MIB_IPNETTABLE));
        status = GetIpNetTable(storage.data(), &bytes, FALSE);
        if (status != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
    }
    if (status == ERROR_NO_DATA) {
        return result;
    }
    if (status != NO_ERROR) {
        result.error = "Cannot read host ARP table (error " + std::to_string(status) + ")";
        return result;
    }
    const auto* table = storage.data();
    const std::size_t table_bytes = storage.size() * sizeof(MIB_IPNETTABLE);
    constexpr std::size_t row_offset = offsetof(MIB_IPNETTABLE, table);
    if (table_bytes < row_offset ||
        table->dwNumEntries > (table_bytes - row_offset) / sizeof(MIB_IPNETROW)) {
        result.error = "Host ARP table data invalid";
        return result;
    }
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        if (result.entries.size() == 32) {
            result.truncated = true;
            break;
        }
        MIB_IPNETROW row{};
        std::memcpy(&row, reinterpret_cast<const unsigned char*>(table) + row_offset +
                              static_cast<std::size_t>(index) * sizeof(row), sizeof(row));
        const auto* octets = reinterpret_cast<const unsigned char*>(&row.dwAddr);
        char address[16]{};
        std::snprintf(address, sizeof(address), "%u.%u.%u.%u",
                      static_cast<unsigned>(octets[0]), static_cast<unsigned>(octets[1]),
                      static_cast<unsigned>(octets[2]), static_cast<unsigned>(octets[3]));
        std::string hardware_address;
        if (row.dwPhysAddrLen > sizeof(row.bPhysAddr)) {
            result.entries.clear();
            result.error = "Host ARP hardware address invalid";
            return result;
        }
        for (DWORD byte = 0; byte < row.dwPhysAddrLen; ++byte) {
            char part[4]{};
            std::snprintf(part, sizeof(part), byte ? ":%02x" : "%02x",
                          static_cast<unsigned>(row.bPhysAddr[byte]));
            hardware_address += part;
        }
        result.entries.push_back({address, hardware_address.empty() ? "Unavailable" : hardware_address,
                                  "Index " + std::to_string(row.dwIndex)});
    }
#else
    result.error = "Host ARP status unsupported on this desktop";
#endif
    return result;
}
}
#else
namespace platform {
namespace {

bool valid_host_argument(const std::string& host) {
    if (host.empty() || host.size() > 253) {
        return false;
    }

    std::size_t label_length = 0;
    bool previous_hyphen = false;
    for (std::size_t index = 0; index < host.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(host[index]);
        if (character == '.') {
            if (label_length == 0 || label_length > 63 || previous_hyphen) {
                return false;
            }
            label_length = 0;
            previous_hyphen = false;
        }
        else if (character == '-') {
            if (label_length == 0) {
                return false;
            }
            ++label_length;
            previous_hyphen = true;
        }
        else if (std::isalnum(character)) {
            ++label_length;
            previous_hyphen = false;
        }
        else {
            return false;
        }
    }
    return label_length > 0 && label_length <= 63 && !previous_hyphen;
}

std::string run_command(const char* command) {
    std::array<char, 256> buffer{};
    std::string output;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command, "r"), pclose);
    if (!pipe) {
        return {};
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        output += buffer.data();
    }
    return output;
}

} // namespace

NetworkInterfacesResult list_network_interfaces() {
    NetworkInterfacesResult result;
    std::error_code error;
    std::filesystem::directory_iterator iterator("/sys/class/net", error);
    const std::filesystem::directory_iterator end;
    if (error) {
        result.error = "Cannot list network interfaces: " + error.message();
        return result;
    }
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            break;
        }
        const auto& entry = *iterator;
        if (entry.path().filename() == "lo") {
            continue;
        }
        NetworkInterface interface;
        interface.name = entry.path().filename().string();
        std::ifstream carrier(entry.path() / "carrier");
        int state = 0;
        interface.state_known = static_cast<bool>(carrier >> state);
        interface.connected = state == 1;
        result.interfaces.push_back(std::move(interface));
    }
    if (error) {
        result.interfaces.clear();
        result.error = "Cannot list network interfaces: " + error.message();
    }
    return result;
}

std::vector<WifiNetwork> scan_wifi_networks() {
    std::vector<WifiNetwork> networks;
    const std::string output = run_command(
        "timeout 8s nmcli -t -f SSID,SECURITY,SIGNAL dev wifi list 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto first = line.find(':');
        const auto second = line.rfind(':');
        if (first == std::string::npos || second == std::string::npos || first == second) {
            continue;
        }
        WifiNetwork network;
        network.ssid = line.substr(0, first);
        network.security = line.substr(first + 1, second - first - 1);
        try {
            network.signal = std::stoi(line.substr(second + 1));
        } catch (...) {
            network.signal = -1;
        }
        if (!network.ssid.empty()) {
            networks.push_back(std::move(network));
        }
    }
    return networks;
}

std::string ping_host(const std::string& host) {
    if (!valid_host_argument(host)) {
        return "Invalid host";
    }
    const std::string command = "timeout 5s ping -c 2 -W 2 \"" + host + "\" 2>&1";
    return run_command(command.c_str());
}

std::string dns_lookup(const std::string& host) {
    if (!valid_host_argument(host)) {
        return "Invalid host";
    }
    const std::string command = "timeout 5s getent ahosts \"" + host + "\" 2>&1";
    return run_command(command.c_str());
}

std::vector<std::string> scan_bluetooth_devices() {
    std::vector<std::string> devices;
    const std::string output = run_command(
        "timeout 8s bluetoothctl --timeout 6 scan on 2>/dev/null");
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("Device ") != std::string::npos) {
            devices.push_back(line);
            if (devices.size() >= 32) {
                break;
            }
        }
    }
    return devices;
}

std::string default_gateway() {
    std::ifstream route("/proc/net/route");
    return parse_default_gateway(route);
}

ArpResult arp_entries() {
    std::ifstream arp("/proc/net/arp");
    return parse_arp_table(arp);
}

} // namespace platform
#endif
