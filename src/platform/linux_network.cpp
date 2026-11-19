#include "linux_network.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#if USE_DESKTOP
namespace platform {
std::vector<NetworkInterface> list_network_interfaces() {
    return {{"emulator", "127.0.0.1", true}};
}
std::vector<WifiNetwork> scan_wifi_networks() {
    return {{"Emulator Wi-Fi", "WPA2", 75}};
}
std::string ping_host(const std::string&) { return "Desktop emulator: ping disabled"; }
std::string dns_lookup(const std::string&) { return "Desktop emulator: DNS disabled"; }
std::vector<std::string> scan_bluetooth_devices() { return {"Desktop emulator: no BLE data"}; }
std::string default_gateway() { return "Desktop emulator"; }
std::vector<std::string> arp_entries() { return {"Desktop emulator"}; }
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

std::vector<NetworkInterface> list_network_interfaces() {
    std::vector<NetworkInterface> interfaces;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net", error)) {
        if (entry.path().filename() == "lo") {
            continue;
        }
        NetworkInterface interface;
        interface.name = entry.path().filename().string();
        std::ifstream carrier(entry.path() / "carrier");
        int state = 0;
        carrier >> state;
        interface.connected = state == 1;
        interfaces.push_back(std::move(interface));
    }
    return interfaces;
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
    std::string line;
    std::getline(route, line);
    while (std::getline(route, line)) {
        std::istringstream fields(line);
        std::string iface;
        std::string destination;
        std::string gateway;
        fields >> iface >> destination >> gateway;
        if (destination == "00000000") {
            unsigned long raw = std::stoul(gateway, nullptr, 16);
            char text[32];
            std::snprintf(text, sizeof(text),
                          "%lu.%lu.%lu.%lu",
                          raw & 0xff,
                          (raw >> 8) & 0xff,
                          (raw >> 16) & 0xff,
                          (raw >> 24) & 0xff);
            return text;
        }
    }
    return "No gateway";
}

std::vector<std::string> arp_entries() {
    std::vector<std::string> entries;
    std::ifstream arp("/proc/net/arp");
    std::string line;
    std::getline(arp, line);
    while (std::getline(arp, line) && entries.size() < 32) {
        entries.push_back(line);
    }
    return entries;
}

} // namespace platform
#endif
