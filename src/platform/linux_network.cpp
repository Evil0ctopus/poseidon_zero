#include "linux_network.h"

#include <array>
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
}
#else
namespace platform {
namespace {

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
    if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos) {
        return "Invalid host";
    }
    const std::string command = "timeout 5s ping -c 2 -W 2 \"" + host + "\" 2>&1";
    return run_command(command.c_str());
}

std::string dns_lookup(const std::string& host) {
    if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos) {
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

} // namespace platform
#endif
