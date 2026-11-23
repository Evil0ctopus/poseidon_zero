#include "ui_sound.h"

#if defined(POSEIDON_TEST_NULL_AUDIO)
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_NULL
#else
#define MA_NO_NULL
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <chrono>
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

void UiSound::play_music(std::filesystem::path path, int volume) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        music_path_ = std::move(path);
        music_volume_ = std::clamp(volume, 0, 100);
        ++music_generation_;
        music_status_ = {MusicState::Loading, {}};
        music_command_ = MusicCommand::Start;
    }
    condition_.notify_one();
}

void UiSound::stop_music() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++music_generation_;
        music_status_ = {};
        music_command_ = MusicCommand::Stop;
    }
    condition_.notify_one();
}

MusicStatus UiSound::music_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return music_status_;
}

void UiSound::play_tone(double frequency, int volume) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++tone_generation_;
        if (!(frequency >= 20 && frequency <= 20000)) {
            tone_status_ = {MusicState::Error, "Invalid tone frequency"};
            tone_command_ = MusicCommand::Stop;
        } else {
            tone_frequency_ = frequency;
            tone_volume_ = std::clamp(volume, 0, 100);
            tone_status_ = {MusicState::Loading, {}};
            tone_command_ = MusicCommand::Start;
        }
    }
    condition_.notify_one();
}

void UiSound::stop_tone() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++tone_generation_;
        tone_status_ = {};
        tone_command_ = MusicCommand::Stop;
    }
    condition_.notify_one();
}

MusicStatus UiSound::tone_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tone_status_;
}

void UiSound::run() {
    ma_engine engine{};
    ma_sound sound{};
    ma_sound music{};
    ma_waveform waveform{};
    ma_sound tone{};
#if defined(POSEIDON_TEST_NULL_AUDIO)
    ma_context context{};
    bool context_ready = false;
#endif
    bool engine_ready = false;
    bool sound_ready = false;
    bool music_ready = false;
    bool waveform_ready = false;
    bool tone_ready = false;
    uint64_t active_generation = 0;
    uint64_t active_tone_generation = 0;
    std::chrono::steady_clock::time_point tone_deadline;
    const auto initialize_engine = [&]() {
#if defined(POSEIDON_TEST_NULL_AUDIO)
        if (!context_ready) {
            const ma_backend backend = ma_backend_null;
            const auto context_result = ma_context_init(&backend, 1, nullptr, &context);
            if (context_result != MA_SUCCESS) return context_result;
            context_ready = true;
        }
        ma_engine_config config = ma_engine_config_init();
        config.pContext = &context;
        return ma_engine_init(&config, &engine);
#else
        return ma_engine_init(nullptr, &engine);
#endif
    };
    for (;;) {
        int volume = 0;
        int music_volume = 0;
        bool play_click = false;
        MusicCommand command = MusicCommand::None;
        MusicCommand tone_command = MusicCommand::None;
        std::filesystem::path path;
        uint64_t generation = 0;
        uint64_t tone_generation = 0;
        double tone_frequency = 0;
        int tone_volume = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] {
                return stopping_ || pending_ || music_command_ != MusicCommand::None ||
                       tone_command_ != MusicCommand::None;
            };
            if (tone_ready) {
                condition_.wait_until(lock, tone_deadline, ready);
            } else if (music_ready) {
                condition_.wait_for(lock, std::chrono::milliseconds(150), ready);
            } else {
                condition_.wait(lock, ready);
            }
            if (stopping_) break;
            play_click = pending_;
            pending_ = false;
            volume = volume_;
            command = music_command_;
            music_command_ = MusicCommand::None;
            if (command == MusicCommand::Start) {
                path = music_path_;
                music_volume = music_volume_;
            }
            generation = music_generation_;
            tone_command = tone_command_;
            tone_command_ = MusicCommand::None;
            tone_generation = tone_generation_;
            tone_frequency = tone_frequency_;
            tone_volume = tone_volume_;
        }

        if (tone_ready &&
            (tone_command != MusicCommand::None ||
             std::chrono::steady_clock::now() >= tone_deadline)) {
            ma_sound_uninit(&tone);
            ma_waveform_uninit(&waveform);
            tone_ready = false;
            waveform_ready = false;
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_tone_generation == tone_generation_ && tone_command == MusicCommand::None) {
                tone_status_ = {MusicState::Finished, {}};
            }
        }
        if (tone_command == MusicCommand::Start) {
            ma_result result = MA_SUCCESS;
            if (!engine_ready) {
                result = initialize_engine();
                engine_ready = result == MA_SUCCESS;
            }
            if (result == MA_SUCCESS) {
                auto config = ma_waveform_config_init(ma_format_f32, 1, 48000,
                                                      ma_waveform_type_sine, 0.25, tone_frequency);
                result = ma_waveform_init(&config, &waveform);
                waveform_ready = result == MA_SUCCESS;
            }
            if (result == MA_SUCCESS) {
                result = ma_sound_init_from_data_source(&engine, &waveform, 0, nullptr, &tone);
                tone_ready = result == MA_SUCCESS;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tone_generation == tone_generation_ && !stopping_) {
                    if (result == MA_SUCCESS) {
                        ma_sound_set_volume(&tone, static_cast<float>(tone_volume) / 100.0f);
                        result = ma_sound_start(&tone);
                    }
                    tone_status_ = result == MA_SUCCESS
                                       ? MusicStatus{MusicState::Playing, {}}
                                       : MusicStatus{MusicState::Error,
                                             std::string("Tone playback failed: ") +
                                                 ma_result_description(result)};
                    active_tone_generation = tone_generation;
                    tone_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(300);
                } else {
                    result = MA_CANCELLED;
                }
            }
            if (result != MA_SUCCESS) {
                if (tone_ready) ma_sound_uninit(&tone);
                if (waveform_ready) ma_waveform_uninit(&waveform);
                tone_ready = false;
                waveform_ready = false;
            }
        }
        if (command != MusicCommand::None && music_ready) {
            ma_sound_uninit(&music);
            music_ready = false;
        }
        if (command == MusicCommand::Start) {
            ma_result result = MA_SUCCESS;
            std::string file_error;
            std::error_code file_status_error;
            const auto file_status = std::filesystem::status(path, file_status_error);
            if (file_status_error) {
                file_error = "Cannot inspect audio file: " + file_status_error.message();
            } else if (!std::filesystem::is_regular_file(file_status)) {
                file_error = "Audio playback requires a regular file";
            }
            if (file_error.empty() && !engine_ready) {
                result = initialize_engine();
                engine_ready = result == MA_SUCCESS;
            }
            if (file_error.empty() && result == MA_SUCCESS) {
#if defined(_WIN32)
                result = ma_sound_init_from_file_w(&engine, path.c_str(), MA_SOUND_FLAG_STREAM,
                                                   nullptr, nullptr, &music);
#else
                result = ma_sound_init_from_file(&engine, path.c_str(), MA_SOUND_FLAG_STREAM,
                                                 nullptr, nullptr, &music);
#endif
                music_ready = result == MA_SUCCESS;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation == music_generation_ && !stopping_) {
                    if (file_error.empty() && result == MA_SUCCESS) {
                        ma_sound_set_volume(&music, static_cast<float>(music_volume) / 100.0f);
                        result = ma_sound_start(&music);
                    }
                    music_status_ = !file_error.empty()
                                        ? MusicStatus{MusicState::Error, file_error}
                                        : result == MA_SUCCESS
                                        ? MusicStatus{MusicState::Playing, {}}
                                        : MusicStatus{MusicState::Error,
                                              std::string("Audio playback failed: ") +
                                                  ma_result_description(result)};
                    active_generation = generation;
                } else if (music_ready) {
                    ma_sound_uninit(&music);
                    music_ready = false;
                }
            }
            if (result != MA_SUCCESS && music_ready) {
                ma_sound_uninit(&music);
                music_ready = false;
            }
        }
        if (music_ready && !ma_sound_is_playing(&music)) {
            const bool finished = ma_sound_at_end(&music) != MA_FALSE;
            ma_sound_uninit(&music);
            music_ready = false;
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_generation == music_generation_) {
                music_status_ = finished ? MusicStatus{MusicState::Finished, {}}
                                         : MusicStatus{MusicState::Error,
                                                       "Audio playback stopped unexpectedly"};
            }
        }
        if (!play_click || music_ready || tone_ready) continue;
        if (volume == 0) {
            if (sound_ready) ma_sound_stop(&sound);
            continue;
        }
        ma_result result = MA_SUCCESS;
        if (!engine_ready) {
            result = initialize_engine();
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
    if (tone_ready) ma_sound_uninit(&tone);
    if (waveform_ready) ma_waveform_uninit(&waveform);
    if (music_ready) ma_sound_uninit(&music);
    if (sound_ready) ma_sound_uninit(&sound);
    if (engine_ready) ma_engine_uninit(&engine);
#if defined(POSEIDON_TEST_NULL_AUDIO)
    if (context_ready) ma_context_uninit(&context);
#endif
}

} // namespace platform
