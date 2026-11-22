#include "ui_sound.h"
#include "microphone.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool write_silent_wav(const std::filesystem::path& path, uint32_t samples) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const auto write_u16 = [&out](uint16_t value) {
        out.put(static_cast<char>(value & 0xffU));
        out.put(static_cast<char>((value >> 8) & 0xffU));
    };
    const auto write_u32 = [&out](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.put(static_cast<char>((value >> shift) & 0xffU));
        }
    };
    const uint32_t bytes = samples * 2U;
    out.write("RIFF", 4);
    write_u32(36U + bytes);
    out.write("WAVEfmt ", 8);
    write_u32(16);
    write_u16(1);
    write_u16(1);
    write_u32(8000);
    write_u32(16000);
    write_u16(2);
    write_u16(16);
    out.write("data", 4);
    write_u32(bytes);
    const std::string silence(bytes, '\0');
    out.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    out.close();
    return out.good();
}

bool wait_for(platform::UiSound& audio, platform::MusicState state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = audio.music_status();
        if (status.state == state) return true;
        if (status.state == platform::MusicState::Error && state != platform::MusicState::Error) {
            std::cerr << status.error << '\n';
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cerr << "Timed out waiting for music state\n";
    return false;
}

} // namespace

int main() {
    const std::atomic_bool cancelled{true};
    if (platform::measure_microphone(cancelled, std::chrono::milliseconds(100)).frames != 0) {
        std::cerr << "Cancelled microphone test must not capture\n";
        return 1;
    }
    const std::atomic_bool running{false};
    if (platform::measure_microphone(running, std::chrono::milliseconds(0)).error.empty()) {
        std::cerr << "Invalid microphone duration must fail\n";
        return 1;
    }
    const auto microphone = platform::measure_microphone(running, std::chrono::milliseconds(300));
    if (!microphone.error.empty() || microphone.frames == 0 || microphone.peak_percent != 0) {
        std::cerr << "Null microphone test failed: " << microphone.error << '\n';
        return 1;
    }
    std::atomic_bool stop_capture{false};
    std::thread cancel_thread([&stop_capture] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop_capture = true;
    });
    const auto stopped = platform::measure_microphone(stop_capture, std::chrono::seconds(5));
    cancel_thread.join();
    if (stopped.frames != 0 || !stopped.error.empty()) {
        std::cerr << "Cancelled microphone test must discard its result\n";
        return 1;
    }
    if (!platform::record_microphone(cancelled, std::chrono::milliseconds(100)).samples.empty()) {
        std::cerr << "Cancelled recording must not retain audio\n";
        return 1;
    }
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error) {
        std::cerr << "Cannot find temporary directory: " << error.message() << '\n';
        return 1;
    }
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = root / ("poseidon-music-test-" + std::to_string(unique));
    if (!std::filesystem::create_directory(directory, error)) {
        std::cerr << "Cannot create temporary directory: " << error.message() << '\n';
        return 1;
    }
    const auto recording = platform::record_microphone(running, std::chrono::milliseconds(300));
    bool passed = recording.error.empty() && !recording.samples.empty() &&
                  recording.samples.size() <= 4800;
    if (!passed) std::cerr << "Null recording failed: " << recording.error << '\n';
    const auto saved = platform::save_recording(directory, recording.samples);
    const auto saved_again = platform::save_recording(directory, recording.samples);
    const auto signed_saved = platform::save_recording(directory, {-32768, -1, 0, 1, 32767});
    if (!saved.error.empty() || !saved_again.error.empty() || saved.path == saved_again.path) {
        std::cerr << "Recording save must create unique files: " << saved.error << '\n';
        passed = false;
    } else {
        std::ifstream input(saved.path, std::ios::binary);
        char header[12]{};
        input.read(header, sizeof(header));
        if (!input || std::string(header, 4) != "RIFF" ||
            std::string(header + 8, 4) != "WAVE" ||
            std::filesystem::file_size(saved.path) != 44 + recording.samples.size() * 2) {
            std::cerr << "Saved recording must contain a complete WAV file\n";
            passed = false;
        }
        if (signed_saved.error.empty()) {
            std::ifstream input(signed_saved.path, std::ios::binary);
            input.seekg(44);
            char bytes[10]{};
            input.read(bytes, sizeof(bytes));
            const unsigned char expected[] = {0, 128, 255, 255, 0, 0, 1, 0, 255, 127};
            for (std::size_t i = 0; i < sizeof(bytes); ++i) {
                if (!input || static_cast<unsigned char>(bytes[i]) != expected[i]) {
                    std::cerr << "Recording PCM must be little-endian signed 16-bit\n";
                    passed = false;
                    break;
                }
            }
        } else {
            std::cerr << "Cannot save signed PCM fixture: " << signed_saved.error << '\n';
            passed = false;
        }
    }
    if (platform::save_recording(directory, {}).error.empty() ||
        platform::save_recording(directory / "missing", recording.samples).error.empty()) {
        std::cerr << "Invalid recording saves must fail\n";
        passed = false;
    }
    const auto track = directory / std::filesystem::u8path(u8"music-\u00e9.wav");
    const auto short_track = directory / "short.wav";
    const bool fixtures_written = write_silent_wav(track, 24000) && write_silent_wav(short_track, 800);
    if (!fixtures_written) std::cerr << "Cannot write audio fixtures\n";
    passed &= fixtures_written;
    if (passed) {
        platform::UiSound audio("");
        audio.play_music(saved.path, 0);
        passed &= wait_for(audio, platform::MusicState::Finished);
        audio.play_music(track, 0);
        passed &= wait_for(audio, platform::MusicState::Playing);
        audio.play(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (audio.music_status().state != platform::MusicState::Playing) {
            std::cerr << "UI sound requests must not stop music\n";
            passed = false;
        }
        audio.stop_music();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (audio.music_status().state != platform::MusicState::Stopped) {
            std::cerr << "Stopped music must not restart\n";
            passed = false;
        }
        audio.play_music(directory / "missing.wav", 0);
        passed &= wait_for(audio, platform::MusicState::Error);
        if (audio.music_status().error.empty()) {
            std::cerr << "Playback failure must include an error\n";
            passed = false;
        }
        audio.play_music(directory, 0);
        passed &= wait_for(audio, platform::MusicState::Error);
        if (audio.music_status().error != "Audio playback requires a regular file") {
            std::cerr << "Non-regular audio paths must be rejected\n";
            passed = false;
        }
        audio.play_music(short_track, 0);
        passed &= wait_for(audio, platform::MusicState::Finished);
        audio.play_music(track, 0);
        audio.stop_music();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (audio.music_status().state != platform::MusicState::Stopped) {
            std::cerr << "Cancelled loading must stay stopped\n";
            passed = false;
        }
        audio.play_tone(440.0, 0);
        const auto tone_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (audio.tone_status().state == platform::MusicState::Loading &&
               std::chrono::steady_clock::now() < tone_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (audio.tone_status().state != platform::MusicState::Playing) {
            std::cerr << "Tone did not start: " << audio.tone_status().error << '\n';
            passed = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        if (audio.tone_status().state != platform::MusicState::Finished) {
            std::cerr << "Tone did not finish\n";
            passed = false;
        }
        audio.play_tone(261.63, 0);
        audio.stop_tone();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        if (audio.tone_status().state != platform::MusicState::Stopped) {
            std::cerr << "Cancelled tone must stay stopped\n";
            passed = false;
        }
        audio.play_tone(0, 0);
        if (audio.tone_status().state != platform::MusicState::Error) {
            std::cerr << "Invalid tone frequency must be rejected\n";
            passed = false;
        }
    }
    std::filesystem::remove(track, error);
    if (error) { std::cerr << "Cannot remove audio fixture: " << error.message() << '\n'; passed = false; }
    std::filesystem::remove(short_track, error);
    if (error) { std::cerr << "Cannot remove audio fixture: " << error.message() << '\n'; passed = false; }
    if (!saved.path.empty()) {
        std::filesystem::remove(saved.path, error);
        if (error) { std::cerr << "Cannot remove recording: " << error.message() << '\n'; passed = false; }
    }
    if (!saved_again.path.empty()) {
        std::filesystem::remove(saved_again.path, error);
        if (error) { std::cerr << "Cannot remove recording: " << error.message() << '\n'; passed = false; }
    }
    if (!signed_saved.path.empty()) {
        std::filesystem::remove(signed_saved.path, error);
        if (error) { std::cerr << "Cannot remove recording: " << error.message() << '\n'; passed = false; }
    }
    std::filesystem::remove(directory, error);
    if (error) { std::cerr << "Cannot remove temporary directory: " << error.message() << '\n'; passed = false; }
    return passed ? 0 : 1;
}
