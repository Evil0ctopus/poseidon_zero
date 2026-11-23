#include "ir_receive.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <unistd.h>

int main() {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        std::cerr << "Cannot create IR fixture pipe\n";
        return 1;
    }
    bool passed = true;
    const std::atomic_bool running{false};
    const uint32_t pulse = 0x01000000U | 9000U;
    if (write(pipe_fds[1], &pulse, sizeof(pulse)) != sizeof(pulse)) {
        std::cerr << "Cannot write IR fixture\n";
        passed = false;
    } else {
        const auto partial = platform::receive_ir_test(pipe_fds[0], running, 150, 1000);
        if (!partial.error.empty() || !partial.truncated || partial.samples.size() != 1 ||
            platform::save_ir_capture("/missing", partial).error !=
                "Only complete IR captures can be saved") {
            std::cerr << "Deadline must mark an unfinished IR signal as unsavable\n";
            passed = false;
        }
    }
    const uint32_t complete[] = {pulse, 0x03000000U};
    if (write(pipe_fds[1], complete, sizeof(complete)) != sizeof(complete)) {
        std::cerr << "Cannot write complete IR fixture\n";
        passed = false;
    } else {
        const auto signal = platform::receive_ir_test(pipe_fds[0], running, 1000, 1000);
        if (!signal.error.empty() || signal.truncated || signal.samples.size() != 1) {
            std::cerr << "A terminated IR signal must remain complete\n";
            passed = false;
        }
    }
    if (write(pipe_fds[1], &pulse, sizeof(pulse)) != sizeof(pulse)) {
        std::cerr << "Cannot write quiet-period IR fixture\n";
        passed = false;
    } else {
        const auto quiet = platform::receive_ir_test(pipe_fds[0], running, 1000, 100);
        if (!quiet.error.empty() || quiet.truncated || quiet.samples.size() != 1) {
            std::cerr << "A signal followed by a quiet period must remain complete\n";
            passed = false;
        }
    }
    const uint32_t overflow[] = {pulse, 0x04000000U};
    if (write(pipe_fds[1], overflow, sizeof(overflow)) != sizeof(overflow)) {
        std::cerr << "Cannot write overflow IR fixture\n";
        passed = false;
    } else {
        const auto failed = platform::receive_ir_test(pipe_fds[0], running, 1000, 1000);
        if (failed.error != "IR receiver overflow" || !failed.samples.empty()) {
            std::cerr << "An IR overflow must discard partial samples\n";
            passed = false;
        }
    }
    const auto empty = platform::receive_ir_test(pipe_fds[0], running, 150, 1000);
    if (empty.error != "No IR signal received" || !empty.samples.empty()) {
        std::cerr << "An empty capture must report no signal\n";
        passed = false;
    }
    const std::atomic_bool cancelled{true};
    const auto stopped = platform::receive_ir_test(pipe_fds[0], cancelled, 1000, 1000);
    if (!stopped.error.empty() || !stopped.samples.empty()) {
        std::cerr << "A cancelled IR capture must not report data or an error\n";
        passed = false;
    }
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return passed ? 0 : 1;
}
