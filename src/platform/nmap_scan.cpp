#include "nmap_scan.h"
#include "port_scan.h"

#include <algorithm>
#include <charconv>
#include <sstream>

#if !USE_DESKTOP
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace platform {
namespace {

std::string safe_text(const std::string& value) {
    std::string result;
    for (unsigned char ch : value) {
        if (result.size() == 120) break;
        result += ch >= 32 && ch <= 126 ? static_cast<char>(ch) : '?';
    }
    return result;
}

bool valid_target(const std::string& address, const std::vector<uint16_t>& ports) {
    if (address.find('/') != std::string::npos || ports.empty() || ports.size() > 8) return false;
    const auto target = numeric_port_targets(address);
    return target.error.empty() && target.addresses.size() == 1 &&
           target.addresses.front() == address &&
           std::all_of(ports.begin(), ports.end(), [](uint16_t port) { return port != 0; });
}

} // namespace

NmapResult parse_nmap_grepable(const std::string& output, const std::string& address,
                               const std::vector<uint16_t>& ports) {
    NmapResult result;
    result.address = address;
    if (!valid_target(address, ports) || output.size() > 128 * 1024) {
        result.error = "Invalid inspection result";
        return result;
    }
    bool found = false;
    std::istringstream input(output);
    std::string line;
    while (std::getline(input, line)) {
        const auto host_prefix = "Host: " + address + " ";
        if (line.compare(0, host_prefix.size(), host_prefix) != 0) continue;
        found = true;
        std::istringstream fields(line);
        std::string field;
        while (std::getline(fields, field, '\t')) {
            if (field.compare(0, 4, "OS: ") == 0) result.os_guess = safe_text(field.substr(4));
            if (field.compare(0, 7, "Ports: ") != 0) continue;
            std::string entries = field.substr(7);
            std::size_t start = 0;
            while (start < entries.size()) {
                auto end = entries.find(", ", start);
                while (end != std::string::npos) {
                    std::size_t digit = end + 2;
                    while (digit < entries.size() && entries[digit] >= '0' && entries[digit] <= '9')
                        ++digit;
                    if (digit > end + 2 && digit < entries.size() && entries[digit] == '/') break;
                    end = entries.find(", ", end + 2);
                }
                const auto item = entries.substr(start, end == std::string::npos ? end : end - start);
                std::vector<std::string> parts;
                std::istringstream pieces(item);
                std::string part;
                while (std::getline(pieces, part, '/')) parts.push_back(part);
                if (parts.size() < 7) {
                    result.error = "Invalid Nmap port output";
                    return result;
                }
                unsigned port = 0;
                const auto parsed = std::from_chars(parts[0].data(),
                                                    parts[0].data() + parts[0].size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != parts[0].data() + parts[0].size() ||
                    port == 0 || port > 65535 ||
                    std::find(ports.begin(), ports.end(), static_cast<uint16_t>(port)) == ports.end() ||
                    parts[2] != "tcp") {
                    result.error = "Unexpected Nmap port output";
                    return result;
                }
                if (parts[1] == "open") {
                    if (result.services.size() >= ports.size() ||
                        std::any_of(result.services.begin(), result.services.end(),
                                    [&](const NmapService& service) { return service.port == port; })) {
                        result.error = "Duplicate Nmap port output";
                        return result;
                    }
                    result.services.push_back({static_cast<uint16_t>(port),
                                               safe_text(parts[4]), safe_text(parts[6])});
                }
                if (end == std::string::npos) break;
                start = end + 2;
            }
        }
    }
    if (!found) result.error = "Nmap returned no result for the selected address";
    return result;
}

static NmapResult run_nmap_impl(const std::string& executable, const std::string& address,
                                const std::vector<uint16_t>& ports, bool os_detection,
                                const std::atomic_bool& cancelled) {
    NmapResult result;
    result.address = address;
    if (!valid_target(address, ports)) { result.error = "Invalid inspection target"; return result; }
    if (cancelled) { result.cancelled = true; return result; }
#if USE_DESKTOP
    result.error = "Nmap inspection requires a supported device";
    return result;
#else
    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        result.error = std::string("Output pipe: ") + std::strerror(errno);
        return result;
    }
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        const int error = errno;
        close(out_pipe[0]); close(out_pipe[1]);
        result.error = std::string("Error pipe: ") + std::strerror(error);
        return result;
    }
    const auto close_pipes = [&] {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
            if (fd >= 0) close(fd);
    };
    if (out_pipe[0] <= 2 || out_pipe[1] <= 2 || err_pipe[0] <= 2 || err_pipe[1] <= 2) {
        close_pipes(); result.error = "Standard descriptors unavailable"; return result;
    }
    for (int fd : {out_pipe[0], err_pipe[0]}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            result.error = std::string("Output setup: ") + std::strerror(errno);
            close_pipes(); return result;
        }
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) { close_pipes(); result.error = std::strerror(error); return result; }
    const auto add = [&error](int value) { if (!error) error = value; };
    add(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    add(posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO));
    add(posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO));
    for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
        add(posix_spawn_file_actions_addclose(&actions, fd));
    posix_spawnattr_t attrs;
    const int attr_error = posix_spawnattr_init(&attrs);
    if (attr_error) {
        posix_spawn_file_actions_destroy(&actions); close_pipes();
        result.error = std::strerror(attr_error); return result;
    }
    add(posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP));
    add(posix_spawnattr_setpgroup(&attrs, 0));
    const std::string port_text = format_port_list(ports);
    std::vector<std::string> args{executable, "-n", "-Pn", "-sT", "-sV", "--version-light",
                                  "--max-retries", "1", "--host-timeout", "60s", "-T2",
                                  "-p", port_text, "-oG", "-"};
    if (address.find(':') != std::string::npos) args.push_back("-6");
    if (os_detection) {
        args.push_back("-O");
        args.push_back("--max-os-tries");
        args.push_back("1");
    }
    args.push_back(address);
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (!error) error = posix_spawn(&pid, args[0].c_str(), &actions, &attrs, argv.data(), environ);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
        close_pipes();
        result.error = error == ENOENT ? "Nmap not installed at /usr/bin/nmap" :
                       std::string("Cannot start Nmap: ") + std::strerror(error);
        return result;
    }
    close(out_pipe[1]); out_pipe[1] = -1;
    close(err_pipe[1]); err_pipe[1] = -1;
    std::string output, diagnostics;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    int status = 0;
    bool exited = false;
    const auto drain = [&](int fd, std::string& text, std::size_t limit) {
        char buffer[2048];
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto count = read(fd, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (count < 0) { result.error = std::string("Cannot read Nmap output: ") + std::strerror(errno); return false; }
            if (!count) return true;
            if (text.size() + static_cast<std::size_t>(count) > limit) {
                result.error = "Nmap output exceeded its limit"; return false;
            }
            text.append(buffer, static_cast<std::size_t>(count));
        }
        return true;
    };
    while (!exited) {
        if (cancelled || std::chrono::steady_clock::now() >= deadline) {
            result.cancelled = cancelled;
            if (!cancelled) result.error = "Nmap inspection timed out";
            break;
        }
        pollfd descriptors[] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
        const int ready = poll(descriptors, 2, 100);
        if (ready < 0 && errno != EINTR) { result.error = "Cannot poll Nmap output"; break; }
        if (!drain(out_pipe[0], output, 128 * 1024) || !drain(err_pipe[0], diagnostics, 8 * 1024))
            break;
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) exited = true;
        else if (waited < 0 && errno != EINTR) {
            result.error = std::string("Cannot wait for Nmap: ") + std::strerror(errno);
            break;
        }
    }
    if (!exited) {
        if (kill(-pid, SIGKILL) != 0 && errno != ESRCH) kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    } else if (drain(out_pipe[0], output, 128 * 1024) &&
               drain(err_pipe[0], diagnostics, 8 * 1024) && result.error.empty()) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            result.error = "Nmap failed: " + safe_text(diagnostics.empty() ? "process error" : diagnostics);
        else {
            result = parse_nmap_grepable(output, address, ports);
            if (!diagnostics.empty()) result.warning = safe_text(diagnostics);
        }
    }
    close_pipes();
    return result;
#endif
}

NmapResult run_nmap_inspection(const std::string& address, const std::vector<uint16_t>& ports,
                               bool os_detection, const std::atomic_bool& cancelled) {
    return run_nmap_impl("/usr/bin/nmap", address, ports, os_detection, cancelled);
}

#if defined(POSEIDON_NMAP_TESTING)
NmapResult run_nmap_test_executable(const std::string& executable, const std::string& address,
                                    const std::vector<uint16_t>& ports, bool os_detection,
                                    const std::atomic_bool& cancelled) {
    return run_nmap_impl(executable, address, ports, os_detection, cancelled);
}
#endif

} // namespace platform
