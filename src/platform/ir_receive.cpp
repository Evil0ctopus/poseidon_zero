#include "ir_receive.h"
#include "unique_file.h"

namespace platform {
namespace {

constexpr uint32_t kModeMask = 0xff000000U;
constexpr uint32_t kValueMask = 0x00ffffffU;
constexpr uint32_t kPulse = 0x01000000U;
constexpr uint32_t kTimeout = 0x03000000U;
constexpr uint32_t kOverflow = 0x04000000U;
constexpr std::size_t kMaxSamples = 128;

} // namespace

IrSampleStatus append_ir_sample(IrReceiveResult& result, uint32_t value) {
    const uint32_t mode = value & kModeMask;
    if (mode == kOverflow) return IrSampleStatus::Overflow;
    if (mode == kTimeout) return result.samples.empty() ? IrSampleStatus::Continue
                                                        : IrSampleStatus::Complete;
    if (mode != 0 && mode != kPulse) return IrSampleStatus::Continue;
    if (result.samples.empty() && mode == 0) return IrSampleStatus::Continue;
    if (result.samples.size() == kMaxSamples) {
        result.truncated = true;
        return IrSampleStatus::Complete;
    }
    result.samples.push_back({value & kValueMask, mode == kPulse});
    return IrSampleStatus::Continue;
}

IrSaveResult save_ir_capture(const std::filesystem::path& directory,
                             const IrReceiveResult& capture) {
    if (!capture.error.empty() || capture.samples.empty() || capture.truncated ||
        capture.samples.size() > kMaxSamples)
        return {{}, "Only complete IR captures can be saved"};
    auto output = open_unique_file(directory, "poseidon-ir-", ".txt");
    if (!output.error.empty()) return {{}, output.error};
    FILE* file = output.stream.get();
    bool ok = std::fputs("POSEIDON_IR_V1\ncarrier_hz=unknown\n", file) >= 0;
    for (const auto& sample : capture.samples) {
        if (!ok) break;
        ok = std::fprintf(file, "%s %u\n", sample.pulse ? "pulse" : "space",
                          static_cast<unsigned>(sample.duration_us)) > 0;
    }
    if (std::fclose(output.stream.release()) != 0) ok = false;
    if (!ok) return {{}, "Could not write IR capture" + remove_incomplete_file(output.path)};
    return {output.path, {}};
}

} // namespace platform

#if USE_DESKTOP

namespace platform {

IrReceiveResult receive_ir(const std::atomic_bool& cancelled) {
    if (cancelled) return {};
    return {{}, false, "IR reception requires a supported device"};
}

} // namespace platform

#else

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/lirc.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace platform {
namespace {

static_assert(kModeMask == LIRC_MODE2_MASK && kValueMask == LIRC_VALUE_MASK &&
              kPulse == LIRC_MODE2_PULSE && kTimeout == LIRC_MODE2_TIMEOUT &&
              kOverflow == LIRC_MODE2_OVERFLOW, "Unexpected LIRC sample format");

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return fd_; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
private:
    int fd_;
};

std::string system_error(const std::string& action, int error) {
    return action + ": " + std::strerror(error);
}

} // namespace

IrReceiveResult receive_ir(const std::atomic_bool& cancelled) {
    IrReceiveResult result;
    std::string open_error;
    int receiver = -1;
    for (int index = 0; index < 64 && !cancelled; ++index) {
        const auto path = "/dev/lirc" + std::to_string(index);
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno != ENOENT && open_error.empty()) open_error = system_error(path, errno);
            continue;
        }
        FileDescriptor candidate(fd);
        uint32_t features = 0;
        const int feature_result = ioctl(candidate.get(), LIRC_GET_FEATURES, &features);
        if (feature_result == 0 && (features & LIRC_CAN_REC_MODE2) != 0) {
            receiver = candidate.release();
            break;
        }
        if (feature_result < 0 && open_error.empty()) {
            open_error = system_error(path + " capabilities", errno);
        }
    }
    FileDescriptor device(receiver);
    if (cancelled) return {};
    if (receiver < 0) {
        result.error = open_error.empty() ? "No raw IR receiver found" : open_error;
        return result;
    }
    uint32_t mode = LIRC_MODE_MODE2;
    if (ioctl(device.get(), LIRC_SET_REC_MODE, &mode) < 0) {
        result.error = system_error("IR receive mode", errno);
        return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    auto last_sample = std::chrono::steady_clock::now();
    while (!cancelled && std::chrono::steady_clock::now() < deadline) {
        pollfd event{device.get(), POLLIN, 0};
        const int ready = poll(&event, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            result.error = system_error("IR polling", errno);
            break;
        }
        if (ready == 0) {
            if (!result.samples.empty() &&
                std::chrono::steady_clock::now() - last_sample >= std::chrono::milliseconds(200)) {
                return result;
            }
            continue;
        }
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            result.error = "IR receiver disconnected";
            break;
        }
        uint32_t values[64];
        const auto bytes = read(device.get(), values, sizeof(values));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            result.error = system_error("IR reading", errno);
            break;
        }
        if (bytes == 0 || bytes % sizeof(values[0]) != 0) {
            result.error = "Invalid IR receiver data";
            break;
        }
        last_sample = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < static_cast<std::size_t>(bytes) / sizeof(values[0]); ++i) {
            const auto status = append_ir_sample(result, values[i]);
            if (status == IrSampleStatus::Complete) return result;
            if (status == IrSampleStatus::Overflow) {
                result.error = "IR receiver overflow";
                break;
            }
        }
        if (!result.error.empty()) break;
    }
    if (cancelled) return {};
    if (result.error.empty() && result.samples.empty()) result.error = "No IR signal received";
    if (!result.error.empty()) result.samples.clear();
    return result;
}

} // namespace platform

#endif
