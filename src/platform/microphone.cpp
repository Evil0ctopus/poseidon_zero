#include "microphone.h"
#include "unique_file.h"

#if defined(POSEIDON_TEST_NULL_AUDIO)
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_NULL
#else
#define MA_NO_NULL
#endif
#include "miniaudio.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <thread>

namespace platform {
namespace {

constexpr uint32_t kSampleRate = 16000;

struct CaptureState {
    std::atomic<uint64_t> frames{0};
    std::atomic<int> peak{0};
    std::vector<int16_t> samples;
};

void on_capture(ma_device* device, void*, const void* input, ma_uint32 frames) {
    if (!input) return;
    auto& state = *static_cast<CaptureState*>(device->pUserData);
    const auto* samples = static_cast<const int16_t*>(input);
    int peak = 0;
    for (ma_uint32 i = 0; i < frames; ++i) {
        const int sample = samples[i];
        peak = std::max(peak, sample < 0 ? -sample : sample);
    }
    const int previous = state.peak.load(std::memory_order_relaxed);
    if (peak > previous) state.peak.store(peak, std::memory_order_relaxed);
    if (!state.samples.empty()) {
        const auto offset = state.frames.load(std::memory_order_relaxed);
        const auto available = offset < state.samples.size() ? state.samples.size() - offset : 0;
        std::copy_n(samples, std::min<std::size_t>(frames, available),
                    state.samples.begin() + std::min<std::size_t>(offset, state.samples.size()));
    }
    state.frames.fetch_add(frames, std::memory_order_relaxed);
}

struct CaptureResult {
    MicrophoneLevel level;
    std::vector<int16_t> samples;
};

CaptureResult capture(const std::atomic_bool& cancelled, std::chrono::milliseconds duration,
                      bool keep_samples) {
    if (cancelled) return {};
    if (duration <= std::chrono::milliseconds::zero() || duration > std::chrono::seconds(60))
        return {{0, 0, "Invalid microphone test duration"}, {}};

    CaptureState state;
    if (keep_samples) {
        const auto frames = duration.count() * kSampleRate / 1000;
        state.samples.resize(static_cast<std::size_t>(frames));
    }
#if defined(POSEIDON_TEST_NULL_AUDIO)
    ma_context context{};
    const ma_backend backend = ma_backend_null;
    const auto context_result = ma_context_init(&backend, 1, nullptr, &context);
    if (context_result != MA_SUCCESS)
        return {{0, 0, std::string("Audio context: ") + ma_result_description(context_result)}, {}};
    ma_context* context_ptr = &context;
#else
    ma_context* context_ptr = nullptr;
#endif
    auto config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = kSampleRate;
    config.dataCallback = on_capture;
    config.pUserData = &state;
    ma_device device{};
    auto result = ma_device_init(context_ptr, &config, &device);
    bool initialized = result == MA_SUCCESS;
    if (initialized && !cancelled) result = ma_device_start(&device);
    if (result == MA_SUCCESS && !cancelled) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (!cancelled && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (!ma_device_is_started(&device)) {
                result = MA_DEVICE_NOT_STARTED;
                break;
            }
        }
        if (ma_device_is_started(&device)) {
            const auto stop_result = ma_device_stop(&device);
            if (result == MA_SUCCESS) result = stop_result;
        }
    }
    if (initialized) ma_device_uninit(&device);
#if defined(POSEIDON_TEST_NULL_AUDIO)
    ma_context_uninit(&context);
#endif
    if (cancelled) return {};
    if (result != MA_SUCCESS)
        return {{0, 0, std::string("Microphone capture failed: ") +
                           ma_result_description(result)}, {}};
    const auto frames = state.frames.load(std::memory_order_relaxed);
    if (frames == 0) return {{0, 0, "No microphone samples received"}, {}};
    if (keep_samples)
        state.samples.resize(std::min<uint64_t>(frames, state.samples.size()));
    return {{state.peak.load(std::memory_order_relaxed) * 100 / 32768, frames, {}},
            std::move(state.samples)};
}

bool write_u16(FILE* file, uint16_t value) {
    const unsigned char bytes[] = {static_cast<unsigned char>(value),
                                   static_cast<unsigned char>(value >> 8)};
    return std::fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

bool write_u32(FILE* file, uint32_t value) {
    return write_u16(file, static_cast<uint16_t>(value)) &&
           write_u16(file, static_cast<uint16_t>(value >> 16));
}

} // namespace

MicrophoneLevel measure_microphone(const std::atomic_bool& cancelled,
                                   std::chrono::milliseconds duration) {
    return capture(cancelled, duration, false).level;
}

MicrophoneRecording record_microphone(const std::atomic_bool& cancelled,
                                       std::chrono::milliseconds duration) {
    auto result = capture(cancelled, duration, true);
    return {std::move(result.samples), std::move(result.level.error)};
}

RecordingSaveResult save_recording(const std::filesystem::path& directory,
                                   const std::vector<int16_t>& samples) {
    if (samples.empty() || samples.size() > (std::numeric_limits<uint32_t>::max() - 36U) / 2U)
        return {{}, "Invalid recording length"};
    auto output = open_unique_file(directory, "poseidon-recording-", ".wav");
    if (!output.error.empty()) return {{}, output.error};
    FILE* file = output.stream.get();
    const auto data_size = static_cast<uint32_t>(samples.size() * 2U);
    bool ok = std::fwrite("RIFF", 1, 4, file) == 4 &&
              write_u32(file, 36U + data_size) &&
              std::fwrite("WAVEfmt ", 1, 8, file) == 8 &&
              write_u32(file, 16) && write_u16(file, 1) && write_u16(file, 1) &&
              write_u32(file, kSampleRate) && write_u32(file, kSampleRate * 2) &&
              write_u16(file, 2) && write_u16(file, 16) &&
              std::fwrite("data", 1, 4, file) == 4 && write_u32(file, data_size);
    for (const int16_t sample : samples) {
        if (!ok) break;
        ok = write_u16(file, static_cast<uint16_t>(sample));
    }
    if (std::fclose(output.stream.release()) != 0) ok = false;
    if (!ok) {
        return {{}, "Could not write recording" + remove_incomplete_file(output.path)};
    }
    return {output.path, {}};
}

} // namespace platform
