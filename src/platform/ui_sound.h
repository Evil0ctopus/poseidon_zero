#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

class UiSound {
public:
    explicit UiSound(std::string path);
    ~UiSound();
    UiSound(const UiSound&) = delete;
    UiSound& operator=(const UiSound&) = delete;

    void play(int volume);
    void retry();
    std::string take_error();

private:
    void run();

    std::string path_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_{false};
    bool pending_{false};
    bool failed_{false};
    int volume_{50};
    std::string error_;
    std::thread thread_;
};

} // namespace platform
