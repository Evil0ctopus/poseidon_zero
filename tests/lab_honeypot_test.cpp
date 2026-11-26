#include "lab_honeypot.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

int main() {
    for (const auto* address : {"10.0.0.1", "172.16.0.1", "172.31.255.1",
                                "192.168.1.1", "169.254.1.2", "127.0.0.1"}) {
        if (!platform::valid_lab_honeypot_address(address)) {
            std::cerr << "Rejected valid lab address: " << address << '\n';
            return 1;
        }
    }
    for (const auto* address : {"", "0.0.0.0", "127.0.0.2", "172.15.0.1", "172.32.0.1",
                                "8.8.8.8", "192.168.01.1", "192.168.1.256",
                                "192.168.1.1x", "999999999999999999999999.0.0.0"}) {
        if (platform::valid_lab_honeypot_address(address)) {
            std::cerr << "Accepted invalid lab address: " << address << '\n';
            return 1;
        }
    }
    std::string error;
    if (platform::LabHoneypot::start("0.0.0.0", {}, error) || error.empty()) {
        std::cerr << "Wildcard listener was not rejected\n";
        return 1;
    }
#if USE_DESKTOP
    if (platform::LabHoneypot::start("127.0.0.1", {}, error) || error.empty()) {
        std::cerr << "Desktop listener must report unsupported\n";
        return 1;
    }
#else
    const auto root = std::filesystem::temp_directory_path() /
        ("poseidon-lab-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto session = platform::LabHoneypot::start("127.0.0.1", root, error);
    if (!session) {
        std::cerr << "Start failed: " << error << '\n';
        return 1;
    }
    platform::LabHoneypotStatus status;
    for (int i = 0; i < 200; ++i) {
        status = session->status();
        if (status.listening || status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!status.listening || !status.error.empty() || status.file_name.empty()) {
        std::cerr << "Listener did not start: " << status.error << '\n';
        return 1;
    }
    auto conflict = platform::LabHoneypot::start("127.0.0.1", root, error);
    if (!conflict) {
        std::cerr << "Could not start conflict test: " << error << '\n';
        return 1;
    }
    platform::LabHoneypotStatus conflict_status;
    for (int i = 0; i < 200; ++i) {
        conflict_status = conflict->status();
        if (conflict_status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!conflict_status.complete || conflict_status.error.find("Bind:") != 0) {
        std::cerr << "Port conflict was not reported\n";
        return 1;
    }
    conflict.reset();
    session->stop();
    session.reset();
    std::ifstream log(root / status.file_name);
    const std::string contents{std::istreambuf_iterator<char>(log), {}};
    log.close();
    if (contents.find("Bind: 127.0.0.1:8088") == std::string::npos ||
        contents.find("No payload or credentials") == std::string::npos) {
        std::cerr << "Unexpected listener log\n";
        return 1;
    }
    std::filesystem::remove(root / status.file_name);
    std::filesystem::remove(root);
#endif
    return 0;
}
