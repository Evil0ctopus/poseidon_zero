#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

struct MicrophoneLevel {
    int peak_percent{0};
    uint64_t frames{0};
    std::string error;
};

MicrophoneLevel measure_microphone(const std::atomic_bool& cancelled,
                                   std::chrono::milliseconds duration);

struct MicrophoneRecording {
    std::vector<int16_t> samples;
    std::string error;
};

struct RecordingSaveResult {
    std::filesystem::path path;
    std::string error;
};

MicrophoneRecording record_microphone(const std::atomic_bool& cancelled,
                                      std::chrono::milliseconds duration);
RecordingSaveResult save_recording(const std::filesystem::path& directory,
                                   const std::vector<int16_t>& samples);

} // namespace platform
