#include "camera_capture.h"

#include <cstdio>
#include <jpeglib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

bool script(const fs::path& path, const std::string& body) {
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body << "\n";
    out.close();
    if (!out) return false;
    std::error_code error;
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, error);
    return !error;
}

bool jpeg(const fs::path& path) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    jpeg_compress_struct encoder{};
    jpeg_error_mgr error{};
    encoder.err = jpeg_std_error(&error);
    jpeg_create_compress(&encoder);
    jpeg_stdio_dest(&encoder, file);
    encoder.image_width = 2;
    encoder.image_height = 1;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_start_compress(&encoder, TRUE);
    unsigned char pixels[] = {255, 0, 0, 255, 0, 0};
    JSAMPROW row[] = {pixels};
    jpeg_write_scanlines(&encoder, row, 1);
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    return std::fclose(file) == 0;
}

int main() {
    char name[] = "/tmp/poseidon-camera-test-XXXXXX";
    const char* made = mkdtemp(name);
    if (!made) {
        std::cerr << "Cannot create camera fixture directory\n";
        return 1;
    }
    const fs::path root(made);
    const auto executable = root / "camera";
    const auto image_path = root / "image.jpg";
    bool passed = jpeg(image_path) &&
                  script(executable, "cat \"$(dirname \"$0\")/image.jpg\"");
    const std::atomic_bool running{false};
    if (passed) {
        const auto image = platform::capture_camera_test(running, executable.c_str(), 5000);
        if (!image.error.empty() || image.width != 2 || image.height != 1 ||
            image.pixels.size() != 8 || image.pixels[2] < 200) {
            std::cerr << "Camera fixture did not decode: " << image.error << '\n';
            passed = false;
        }
        if (!script(executable, "echo 'camera denied' >&2; exit 1")) passed = false;
        const auto failure = platform::capture_camera_test(running, executable.c_str(), 5000);
        if (failure.error.find("camera denied") == std::string::npos) {
            std::cerr << "Camera process errors must be shown\n";
            passed = false;
        }
        if (!script(executable, "exit 0")) passed = false;
        const auto empty = platform::capture_camera_test(running, executable.c_str(), 5000);
        if (empty.error.find("no image") == std::string::npos) {
            std::cerr << "Empty camera output must fail\n";
            passed = false;
        }
        if (!script(executable, "printf 'not a JPEG'")) passed = false;
        const auto invalid = platform::capture_camera_test(running, executable.c_str(), 5000);
        if (invalid.error.empty()) {
            std::cerr << "Invalid camera output must fail\n";
            passed = false;
        }
        if (!script(executable, "exec /bin/sleep 10")) passed = false;
        const auto timeout = platform::capture_camera_test(running, executable.c_str(), 200);
        if (timeout.error.find("timed out") == std::string::npos) {
            std::cerr << "Camera timeout was not reported\n";
            passed = false;
        }
        std::atomic_bool cancelled{false};
        std::thread cancel([&cancelled] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cancelled = true;
        });
        const auto cancelled_result = platform::capture_camera_test(cancelled, executable.c_str(), 5000);
        cancel.join();
        if (!cancelled_result.error.empty() || !cancelled_result.pixels.empty()) {
            std::cerr << "Cancelled camera capture must discard its image\n";
            passed = false;
        }
        if (!script(executable, "head -c 9000000 /dev/zero")) passed = false;
        const auto oversized = platform::capture_camera_test(running, executable.c_str(), 10000);
        if (oversized.error.find("8 MiB") == std::string::npos) {
            std::cerr << "Camera output size must be bounded\n";
            passed = false;
        }
        const auto missing = platform::capture_camera_test(running, "/missing/poseidon-camera", 1000);
        if (missing.error.empty()) {
            std::cerr << "Missing camera tool must fail\n";
            passed = false;
        }
    }
    std::error_code cleanup_error;
    fs::remove(executable, cleanup_error);
    if (cleanup_error) passed = false;
    fs::remove(image_path, cleanup_error);
    if (cleanup_error) passed = false;
    fs::remove(root, cleanup_error);
    if (cleanup_error) passed = false;
    return passed ? 0 : 1;
}
