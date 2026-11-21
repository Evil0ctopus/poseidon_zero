#include "ui_sound.h"

#define MA_NO_CAPTURE
#define MA_NO_NULL
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <utility>

namespace platform {

UiSound::UiSound(std::string path)
    : path_(std::move(path)), thread_(&UiSound::run, this) {}

UiSound::~UiSound() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    thread_.join();
}

void UiSound::play(int volume) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_ && volume > 0) return;
        volume_ = std::clamp(volume, 0, 100);
        pending_ = true;
    }
    condition_.notify_one();
}

void UiSound::retry() {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = false;
    error_.clear();
}

std::string UiSound::take_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(error_, {});
}

void UiSound::run() {
    ma_engine engine{};
    ma_sound sound{};
    bool engine_ready = false;
    bool sound_ready = false;
    for (;;) {
        int volume = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || pending_; });
            if (stopping_) break;
            pending_ = false;
            volume = volume_;
        }

        if (volume == 0) {
            if (sound_ready) ma_sound_stop(&sound);
            continue;
        }
        ma_result result = MA_SUCCESS;
        if (!engine_ready) {
            result = ma_engine_init(nullptr, &engine);
            engine_ready = result == MA_SUCCESS;
        }
        if (result == MA_SUCCESS && !sound_ready) {
            result = ma_sound_init_from_file(&engine, path_.c_str(), MA_SOUND_FLAG_DECODE,
                                             nullptr, nullptr, &sound);
            sound_ready = result == MA_SUCCESS;
        }
        if (result == MA_SUCCESS) {
            ma_sound_set_volume(&sound, static_cast<float>(volume) / 100.0f);
            result = ma_sound_seek_to_pcm_frame(&sound, 0);
            if (result == MA_SUCCESS) result = ma_sound_start(&sound);
        }
        if (result != MA_SUCCESS) {
            std::lock_guard<std::mutex> lock(mutex_);
            failed_ = true;
            pending_ = false;
            error_ = "Audio playback failed: " + std::string(ma_result_description(result));
        }
    }
    if (sound_ready) ma_sound_uninit(&sound);
    if (engine_ready) ma_engine_uninit(&engine);
}

} // namespace platform
