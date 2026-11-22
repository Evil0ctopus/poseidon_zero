#include "camera_capture.h"

#if USE_DESKTOP

namespace platform {

GalleryImage capture_camera(const std::atomic_bool& cancelled) {
    if (cancelled) return {};
    return {0, 0, {}, "Camera capture requires a supported device"};
}

} // namespace platform

#else

#include "gallery_jpeg.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace platform {
namespace {

constexpr std::size_t kMaxJpeg = 8U * 1024U * 1024U;
constexpr std::size_t kMaxError = 1024;

class Descriptor {
public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }
private:
    int fd_;
};

struct FileCloser {
    void operator()(FILE* file) const { std::fclose(file); }
};

class Child {
public:
    explicit Child(pid_t pid) : pid_(pid) {}
    ~Child() { stop(); }
    bool poll_exit(int& status, std::string& error) {
        if (pid_ < 0) return true;
        const auto result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return true;
        }
        if (result < 0 && errno != EINTR) {
            error = std::string("Cannot wait for camera: ") + std::strerror(errno);
            if (errno == ECHILD) pid_ = -1;
        }
        return false;
    }
    void stop() {
        if (pid_ < 0) return;
        int status = 0;
        const auto initial = waitpid(pid_, &status, WNOHANG);
        if (initial == pid_ || (initial < 0 && errno == ECHILD)) {
            pid_ = -1;
            return;
        }
        kill(pid_, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            const auto result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(pid_, SIGKILL);
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
private:
    pid_t pid_;
};

std::string safe_error(const std::string& value) {
    std::string result;
    for (unsigned char ch : value) {
        if (result.size() >= 160) break;
        result += ch >= 32 && ch <= 126 && ch != '#' ? static_cast<char>(ch) : ' ';
    }
    return result;
}

GalleryImage capture(const std::atomic_bool& cancelled, const char* executable, int timeout_ms) {
    if (cancelled) return {};
    int image_pipe[2];
    if (pipe2(image_pipe, O_CLOEXEC) != 0)
        return {0, 0, {}, std::string("Camera pipe: ") + std::strerror(errno)};
    Descriptor image_read(image_pipe[0]), image_write(image_pipe[1]);
    int error_pipe[2];
    if (pipe2(error_pipe, O_CLOEXEC) != 0)
        return {0, 0, {}, std::string("Camera error pipe: ") + std::strerror(errno)};
    Descriptor error_read(error_pipe[0]), error_write(error_pipe[1]);
    if (image_read.get() <= STDERR_FILENO || image_write.get() <= STDERR_FILENO ||
        error_read.get() <= STDERR_FILENO || error_write.get() <= STDERR_FILENO)
        return {0, 0, {}, "Camera requires standard input and output"};
    for (const int fd : {image_read.get(), error_read.get()}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return {0, 0, {}, std::string("Camera pipe setup: ") + std::strerror(errno)};
    }
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0) return {0, 0, {}, std::string("Camera setup: ") + std::strerror(result)};
    const auto add = [&result](int next) { if (result == 0) result = next; };
    add(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    add(posix_spawn_file_actions_adddup2(&actions, image_write.get(), STDOUT_FILENO));
    add(posix_spawn_file_actions_adddup2(&actions, error_write.get(), STDERR_FILENO));
    add(posix_spawn_file_actions_addclose(&actions, image_read.get()));
    add(posix_spawn_file_actions_addclose(&actions, image_write.get()));
    add(posix_spawn_file_actions_addclose(&actions, error_read.get()));
    add(posix_spawn_file_actions_addclose(&actions, error_write.get()));
    pid_t pid = -1;
    char arg0[] = "rpicam-still";
    char arg1[] = "--nopreview";
    char arg2[] = "--timeout";
    char arg3[] = "1000";
    char arg4[] = "--width";
    char arg5[] = "320";
    char arg6[] = "--height";
    char arg7[] = "240";
    char arg8[] = "--encoding";
    char arg9[] = "jpg";
    char arg10[] = "--output";
    char arg11[] = "-";
    char* args[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
                    arg8, arg9, arg10, arg11, nullptr};
    if (result == 0) result = posix_spawn(&pid, executable, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0)
        return {0, 0, {}, std::string("Cannot start camera: ") + std::strerror(result)};
    Child child(pid);
    image_write.reset();
    error_write.reset();
    std::vector<unsigned char> bytes;
    std::string details;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool image_open = true, error_open = true, exited = false;
    int status = 0;
    while (!cancelled && std::chrono::steady_clock::now() < deadline) {
        pollfd descriptors[] = {
            {image_open ? image_read.get() : -1, POLLIN, 0},
            {error_open ? error_read.get() : -1, POLLIN, 0},
        };
        const int ready = poll(descriptors, 2, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return {0, 0, {}, std::string("Camera output: ") + std::strerror(errno)};
        }
        for (int i = 0; i < 2; ++i) {
            if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
            if (descriptors[i].revents & POLLNVAL)
                return {0, 0, {}, "Camera output pipe closed unexpectedly"};
            char buffer[4096];
            const auto count = read(descriptors[i].fd, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return {0, 0, {}, std::string("Camera output: ") + std::strerror(errno)};
            }
            if (count == 0) {
                if (i == 0) image_open = false;
                else error_open = false;
            } else if (i == 0) {
                if (bytes.size() + static_cast<std::size_t>(count) > kMaxJpeg)
                    return {0, 0, {}, "Camera image exceeds 8 MiB"};
                bytes.insert(bytes.end(), buffer, buffer + count);
            } else if (details.size() < kMaxError) {
                details.append(buffer, std::min<std::size_t>(count, kMaxError - details.size()));
            }
        }
        std::string wait_error;
        if (!exited) exited = child.poll_exit(status, wait_error);
        if (!wait_error.empty()) return {0, 0, {}, wait_error};
        if (exited && !image_open && !error_open) break;
    }
    if (cancelled) return {};
    if (!exited || image_open || error_open) return {0, 0, {}, "Camera capture timed out"};
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {0, 0, {}, "Camera failed: " +
                               (details.empty() ? std::string("capture process failed")
                                                : safe_error(details))};
    if (bytes.empty()) return {0, 0, {}, "Camera returned no image"};
    std::unique_ptr<FILE, FileCloser> file(fmemopen(bytes.data(), bytes.size(), "rb"));
    if (!file) return {0, 0, {}, "Cannot open camera image in memory"};
    GalleryJpeg image = read_gallery_jpeg(file.get());
    std::unique_ptr<unsigned char, decltype(&std::free)> pixels(image.pixels, &std::free);
    if (image.error[0]) return {0, 0, {}, image.error};
    if (cancelled) return {};
    GalleryImage output;
    output.width = image.width;
    output.height = image.height;
    output.pixels.assign(pixels.get(), pixels.get() +
                         static_cast<std::size_t>(image.width) * image.height * 4U);
    return output;
}

} // namespace

GalleryImage capture_camera(const std::atomic_bool& cancelled) {
    return capture(cancelled, "/usr/bin/rpicam-still", 12000);
}

#if defined(POSEIDON_TEST_CAMERA)
GalleryImage capture_camera_test(const std::atomic_bool& cancelled, const char* executable,
                                 int timeout_ms) {
    return capture(cancelled, executable, timeout_ms);
}
#endif

} // namespace platform

#endif
