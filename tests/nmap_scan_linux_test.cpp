#include "nmap_scan.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
bool write_script(const std::filesystem::path& path, const std::string& body) {
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body;
    out.close();
    return out.good() && chmod(path.c_str(), 0700) == 0;
}
}

int main() {
    char directory[] = "/tmp/poseidon-nmap-test-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    const auto executable = std::filesystem::path(directory) / "fixture";
    const std::atomic_bool not_cancelled{false};
    bool passed = write_script(executable,
        "for expected in '-Pn' '-sT' '--version-light' '22,80' '192.0.2.1'; do\n"
        "  found=0\n"
        "  for actual in \"$@\"; do [ \"$actual\" = \"$expected\" ] && found=1; done\n"
        "  [ \"$found\" = 1 ] || exit 42\n"
        "done\n"
        "printf 'Host: 192.0.2.1 ()\\tPorts: 22/open/tcp//ssh//OpenSSH 9.0/'\n"
        "for actual in \"$@\"; do [ \"$actual\" = '-O' ] && printf '\\tOS: Linux 5.x'; done\n"
        "printf '\\n'\n"
        "printf 'fixture warning' >&2\n");
    const auto result = platform::run_nmap_test_executable(executable.string(), "192.0.2.1",
                                                           {22, 80}, false, not_cancelled);
    passed = passed && result.error.empty() && result.services.size() == 1 &&
             result.services[0].version == "OpenSSH 9.0" &&
             result.warning.find("fixture warning") != std::string::npos;
    const auto os_result = platform::run_nmap_test_executable(executable.string(), "192.0.2.1",
                                                              {22, 80}, true, not_cancelled);
    passed = passed && os_result.error.empty() && os_result.os_guess == "Linux 5.x";
    passed = write_script(executable, "printf 'fixture error' >&2\nexit 42\n") && passed;
    const auto failed = platform::run_nmap_test_executable(executable.string(), "192.0.2.1",
                                                           {22}, false, not_cancelled);
    passed = passed && failed.error.find("fixture error") != std::string::npos;
    passed = write_script(executable, "printf '%0140000d' 0\n") && passed;
    const auto oversized = platform::run_nmap_test_executable(executable.string(), "192.0.2.1",
                                                               {22}, false, not_cancelled);
    passed = passed && oversized.error.find("exceeded") != std::string::npos;
    passed = write_script(executable, "exec /bin/sleep 10\n") && passed;
    std::atomic_bool cancelled{false};
    std::thread stop([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancelled = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const auto interrupted = platform::run_nmap_test_executable(executable.string(), "192.0.2.1",
                                                                {22}, false, cancelled);
    stop.join();
    passed = passed && interrupted.cancelled &&
             std::chrono::steady_clock::now() - start < std::chrono::seconds(5);
    std::error_code error;
    std::filesystem::remove(executable, error);
    if (error) passed = false;
    std::filesystem::remove(directory, error);
    if (error) passed = false;
    if (!passed) std::cerr << "Nmap process fixture failed: " << result.error << '\n';
    return passed ? 0 : 1;
}
