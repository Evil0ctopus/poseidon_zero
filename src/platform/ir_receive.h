#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

struct IrSample {
    uint32_t duration_us;
    bool pulse;
};

struct IrReceiveResult {
    std::vector<IrSample> samples;
    bool truncated{false};
    std::string error;
};

struct IrReceiverStatus {
    bool available{false};
    std::string device_path;
    std::string error;
};

enum class IrSampleStatus {
    Continue,
    Complete,
    Overflow,
};

IrReceiverStatus inspect_ir_receiver();
IrSampleStatus append_ir_sample(IrReceiveResult& result, uint32_t value);
IrReceiveResult receive_ir(const std::atomic_bool& cancelled);

#if defined(POSEIDON_TEST_IR) && !USE_DESKTOP
IrReceiveResult receive_ir_test(int fd, const std::atomic_bool& cancelled,
                                int timeout_ms, int idle_ms);
#endif

struct IrSaveResult {
    std::filesystem::path path;
    std::string error;
};

IrSaveResult save_ir_capture(const std::filesystem::path& directory,
                             const IrReceiveResult& capture);

} // namespace platform
