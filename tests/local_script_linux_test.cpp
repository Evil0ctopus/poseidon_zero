#include "local_script.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

int main() {
    char name[] = "/tmp/poseidon-script-test-XXXXXX";
    if (!mkdtemp(name)) return 1;
    const std::filesystem::path directory(name);
    const auto path = directory / "test.sh";
    { std::ofstream file(path); file << "printf 'reviewed\\n'\n"; }
    const auto script = platform::read_local_script(directory, "test.sh");
    { std::ofstream file(path); file << "printf 'changed\\n'\n"; }
    std::atomic_bool cancelled{false};
    const auto result = platform::run_local_script(script, cancelled);
    bool passed = true;
    const auto check = [&passed](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; passed = false; }
    };
    check(script.error.empty() && result.error.empty() && result.exit_code == 0 &&
          result.output == "reviewed\n", "Snapshot execution failed");
    std::filesystem::create_symlink(path, directory / "link.sh");
    check(!platform::read_local_script(directory, "link.sh").error.empty(), "Symlink accepted");
    check(!platform::read_local_script(directory, "../test.sh").error.empty(), "Traversal accepted");
    { std::ofstream file(path); file << std::string(16385, 'x'); }
    check(!platform::read_local_script(directory, "test.sh").error.empty(), "Oversize script accepted");
    { std::ofstream file(path, std::ios::binary); file.write("a\0b", 3); }
    check(!platform::read_local_script(directory, "test.sh").error.empty(), "Binary script accepted");
    { std::ofstream file(path); file << "exit 7\n"; }
    const auto failing = platform::read_local_script(directory, "test.sh");
    const auto failure = platform::run_local_script(failing, cancelled);
    check(failure.error.empty() && failure.exit_code == 7, "Failure exit code not reported");
    { std::ofstream file(path); file << "yes x | head -c 10000\n"; }
    const auto large = platform::read_local_script(directory, "test.sh");
    const auto large_result = platform::run_local_script(large, cancelled);
    check(large_result.error.empty() && large_result.exit_code == 0 &&
          large_result.output.size() == 8192 && large_result.truncated, "Output truncation failed");
    const auto python_path = directory / "test.py";
    { std::ofstream file(python_path); file << "print('python ok')\n"; }
    const auto python = platform::read_local_script(directory, "test.py");
    const auto python_result = platform::run_local_script(python, cancelled);
    check(python.error.empty() && python_result.error.empty() &&
          python_result.exit_code == 0 && python_result.output == "python ok\n", "Python execution failed");
    { std::ofstream file(path); file << "sleep 10\n"; }
    const auto sleeping = platform::read_local_script(directory, "test.sh");
    std::thread cancel([&cancelled] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancelled = true;
    });
    const auto started = std::chrono::steady_clock::now();
    const auto stopped = platform::run_local_script(sleeping, cancelled);
    cancel.join();
    check(stopped.error == "Canceled" &&
          std::chrono::steady_clock::now() - started < std::chrono::seconds(3), "Cancellation failed");
    std::filesystem::remove(directory / "link.sh");
    std::filesystem::remove(path);
    std::filesystem::remove(python_path);
    std::filesystem::remove(directory);
    if (!passed) std::cerr << "Local script execution test failed\n";
    return passed ? 0 : 1;
}
