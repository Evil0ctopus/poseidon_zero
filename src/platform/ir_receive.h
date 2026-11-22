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

enum class IrSampleStatus {
    Continue,
    Complete,
    Overflow,
};

IrSampleStatus append_ir_sample(IrReceiveResult& result, uint32_t value);
IrReceiveResult receive_ir(const std::atomic_bool& cancelled);

struct IrSaveResult {
    std::filesystem::path path;
    std::string error;
};

IrSaveResult save_ir_capture(const std::filesystem::path& directory,
                             const IrReceiveResult& capture);

} // namespace platform
