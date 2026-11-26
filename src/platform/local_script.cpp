#include "local_script.h"

#if USE_DESKTOP
namespace platform {
LocalScript read_local_script(const std::filesystem::path&, const std::string&) {
    return {{}, {}, false, "Local scripts are unavailable on desktop"};
}
ScriptResult run_local_script(const LocalScript&, const std::atomic_bool&) {
    return {-1, false, {}, "Local scripts are unavailable on desktop"};
}
}
#else

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace platform {
namespace {
constexpr std::size_t kMaxScript = 16 * 1024;
constexpr std::size_t kMaxOutput = 8 * 1024;

class Fd {
public:
    explicit Fd(int value) : value_(value) {}
    ~Fd() { close_now(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    void close_now() { if (value_ >= 0) close(value_); value_ = -1; }
private:
    int value_;
};

std::string error_text(const char* context, int error) {
    return std::string(context) + ": " + std::strerror(error);
}
} // namespace

LocalScript read_local_script(const std::filesystem::path& directory,
                              const std::string& filename) {
    LocalScript result;
    const auto name = std::filesystem::u8path(filename);
    const auto extension = name.extension().string();
    if (filename.empty() || name.filename() != name ||
        (extension != ".sh" && extension != ".py")) {
        result.error = "Select a .sh or .py file";
        return result;
    }
    Fd folder(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (folder.get() < 0) {
        result.error = error_text("Cannot open scripts folder", errno);
        return result;
    }
    Fd file(openat(folder.get(), filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) {
        result.error = error_text("Cannot open script", errno);
        return result;
    }
    struct stat info{};
    if (fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
        result.error = "Script must be a regular file";
        return result;
    }
    char buffer[2048];
    for (;;) {
        const auto count = read(file.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            result.error = error_text("Cannot read script", errno);
            return result;
        }
        if (!count) break;
        if (result.text.size() + static_cast<std::size_t>(count) > kMaxScript) {
            result.error = "Script exceeds 16 KiB";
            return result;
        }
        result.text.append(buffer, static_cast<std::size_t>(count));
    }
    if (result.text.empty()) {
        result.error = "Script is empty";
        return result;
    }
    for (unsigned char ch : result.text) {
        if ((ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') || ch >= 127) {
            result.error = "Preview supports ASCII text only";
            return result;
        }
    }
    result.name = filename;
    result.python = extension == ".py";
    return result;
}

ScriptResult run_local_script(const LocalScript& script, const std::atomic_bool& cancelled) {
    if (!script.error.empty() || script.text.empty()) return {-1, false, {}, "Invalid script"};
    if (cancelled) return {-1, false, {}, "Canceled"};
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) return {-1, false, {}, error_text("Output pipe", errno)};
    Fd input(descriptors[0]), output(descriptors[1]);
    if (input.get() <= STDERR_FILENO || output.get() <= STDERR_FILENO)
        return {-1, false, {}, "Standard descriptors are unavailable"};
    const int flags = fcntl(input.get(), F_GETFL);
    if (flags < 0 || fcntl(input.get(), F_SETFL, flags | O_NONBLOCK) < 0)
        return {-1, false, {}, error_text("Output setup", errno)};
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return {-1, false, {}, error_text("Process setup", error)};
    const auto add = [&error](int value) { if (!error) error = value; };
    add(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    add(posix_spawn_file_actions_adddup2(&actions, output.get(), STDOUT_FILENO));
    add(posix_spawn_file_actions_adddup2(&actions, output.get(), STDERR_FILENO));
    add(posix_spawn_file_actions_addclose(&actions, input.get()));
    add(posix_spawn_file_actions_addclose(&actions, output.get()));
    posix_spawnattr_t attrs;
    const int attr_error = posix_spawnattr_init(&attrs);
    if (attr_error) {
        posix_spawn_file_actions_destroy(&actions);
        return {-1, false, {}, error_text("Process attributes", attr_error)};
    }
    add(posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP));
    add(posix_spawnattr_setpgroup(&attrs, 0));
    const char* executable = script.python ? "/usr/bin/python3" : "/bin/bash";
    char* args[] = {const_cast<char*>(executable), const_cast<char*>("-c"),
                    const_cast<char*>(script.text.c_str()), nullptr};
    pid_t pid = -1;
    if (!error) error = posix_spawn(&pid, executable, &actions, &attrs, args, environ);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    if (error) return {-1, false, {}, error_text("Cannot start interpreter", error)};
    output.close_now();
    ScriptResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    int status = 0;
    bool exited = false;
    const auto drain = [&]() {
        for (int attempt = 0; attempt < 16; ++attempt) {
            char bytes[1024];
            const auto count = read(input.get(), bytes, sizeof(bytes));
            if (count > 0) {
                const auto available = kMaxOutput - result.output.size();
                result.output.append(bytes, std::min<std::size_t>(available, static_cast<std::size_t>(count)));
                if (static_cast<std::size_t>(count) > available) result.truncated = true;
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno != EAGAIN) {
                result.error = error_text("Cannot read output", errno);
                return false;
            }
            break;
        }
        return true;
    };
    while (!exited) {
        if (cancelled || std::chrono::steady_clock::now() >= deadline) {
            result.error = cancelled ? "Canceled" : "Timed out after 60 seconds";
            if (kill(-pid, SIGKILL) != 0 && errno != ESRCH) {
                result.error += "; " + error_text("Cannot stop process group", errno);
                kill(pid, SIGKILL);
            }
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        pollfd fd{input.get(), POLLIN, 0};
        const int ready = poll(&fd, 1, 100);
        if ((ready < 0 && errno != EINTR) ||
            (ready > 0 && (fd.revents & POLLNVAL))) {
            result.error = "Cannot poll script output";
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        if (ready > 0 && (fd.revents & (POLLIN | POLLHUP | POLLERR)) && !drain()) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) exited = true;
        else if (waited < 0 && errno != EINTR) {
            result.error = error_text("Cannot wait for script", errno);
            break;
        }
    }
    if (exited) {
        if (!drain()) return result;
        if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
        else result.error = "Script terminated by signal";
    }
    return result;
}
}
#endif
