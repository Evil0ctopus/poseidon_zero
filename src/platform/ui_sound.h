#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

enum class MusicState {
    Stopped,
    Loading,
    Playing,
    Finished,
    Error,
};

struct MusicStatus {
    MusicState state{MusicState::Stopped};
    std::string error;
};

class UiSound {
public:
    explicit UiSound(std::string path);
    ~UiSound();
    UiSound(const UiSound&) = delete;
    UiSound& operator=(const UiSound&) = delete;

    void play(int volume);
    void retry();
    std::string take_error();
    void play_music(std::filesystem::path path, int volume);
    void stop_music();
    MusicStatus music_status();
    void play_tone(double frequency, int volume);
    void stop_tone();
    MusicStatus tone_status();

private:
    enum class MusicCommand { None, Start, Stop };
    void run();

    std::string path_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_{false};
    bool pending_{false};
    bool failed_{false};
    int volume_{50};
    std::string error_;
    MusicCommand music_command_{MusicCommand::None};
    std::filesystem::path music_path_;
    MusicStatus music_status_;
    uint64_t music_generation_{0};
    int music_volume_{50};
    MusicCommand tone_command_{MusicCommand::None};
    MusicStatus tone_status_;
    uint64_t tone_generation_{0};
    double tone_frequency_{0};
    int tone_volume_{50};
    std::thread thread_;
};

} // namespace platform
