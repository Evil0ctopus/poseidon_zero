#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

bool valid_lab_honeypot_address(const std::string& address);

struct LabHoneypotStatus {
    bool listening{false};
    bool complete{false};
    unsigned connections{0};
    std::string last_peer;
    std::string file_name;
    std::string error;
};

class LabHoneypot {
public:
    static std::unique_ptr<LabHoneypot> start(const std::string& address,
                                              std::filesystem::path directory,
                                              std::string& error);
    ~LabHoneypot();
    void stop();
    LabHoneypotStatus status() const;

    LabHoneypot(const LabHoneypot&) = delete;
    LabHoneypot& operator=(const LabHoneypot&) = delete;

private:
    LabHoneypot() = default;
    void run(std::string address, std::filesystem::path directory);

    std::atomic_bool cancelled_{false};
    mutable std::mutex mutex_;
    LabHoneypotStatus status_;
    std::thread thread_;
};

} // namespace platform
