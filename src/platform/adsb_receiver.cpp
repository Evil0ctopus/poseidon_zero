#include "adsb_receiver.h"

#include "evidence_export.h"
#include "gnss.h"
#include "unique_file.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iomanip>
#include <locale>
#include <sstream>
#include <system_error>

#if !USE_DESKTOP
#include <cerrno>
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

std::unique_ptr<AdsbReceiver> AdsbReceiver::start(std::string& error,
                                                  const std::filesystem::path& log_home,
                                                  const std::string& gps_device) {
    return start_with(kAdsbReceiverExecutable, error, log_home, gps_device);
}

#if defined(POSEIDON_TEST_ADSB)
std::unique_ptr<AdsbReceiver> AdsbReceiver::start_fixture(const std::string& executable,
                                                          std::string& error,
                                                          const std::filesystem::path& log_home,
                                                          const std::string& gps_device) {
    return start_with(executable, error, log_home, gps_device);
}
#endif

std::unique_ptr<AdsbReceiver> AdsbReceiver::start_with(const std::string& executable,
                                                       std::string& error,
                                                       const std::filesystem::path& log_home,
                                                       const std::string& gps_device) {
    error.clear();
    if (!log_home.empty() && !log_home.is_absolute()) {
        error = "ADS-B log home must be absolute";
        return nullptr;
    }
    if (!gps_device.empty() && log_home.empty()) {
        error = "Receiver GPS tagging requires ADS-B logging";
        return nullptr;
    }
#if USE_DESKTOP
    (void)executable;
    (void)log_home;
    (void)gps_device;
    error = "ADS-B reception is unavailable in the simulator";
    return nullptr;
#else
    auto receiver = std::unique_ptr<AdsbReceiver>(new AdsbReceiver);
    try {
        receiver->thread_ = std::thread(&AdsbReceiver::run, receiver.get(), executable, log_home, gps_device);
    } catch (const std::system_error& failure) {
        error = "Cannot start receiver: " + failure.code().message();
        return nullptr;
    }
    return receiver;
#endif
}

AdsbReceiver::~AdsbReceiver() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void AdsbReceiver::stop() { cancelled_ = true; }

AdsbReceiverStatus AdsbReceiver::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void AdsbReceiver::run(std::string executable, std::filesystem::path log_home, std::string gps_device) {
    try {
        run_impl(executable, log_home, gps_device);
    } catch (const std::exception& failure) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.running = false;
        status_.complete = true;
        status_.error = std::string("Receiver failed: ") + failure.what();
    }
}

void AdsbReceiver::run_impl(const std::string& executable, const std::filesystem::path& log_home,
                            const std::string& gps_device) {
#if !USE_DESKTOP
    int output[2]{-1, -1}, errors[2]{-1, -1};
    pid_t pid = -1;
    bool reaped = false;
    struct Cleanup {
        int* output;
        int* errors;
        pid_t& pid;
        bool& reaped;
        ~Cleanup() {
            if (pid > 0 && !reaped) {
                kill(pid, SIGKILL);
                int status = 0;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
            for (int fd : {output[0], output[1], errors[0], errors[1]})
                if (fd >= 0) close(fd);
        }
    } cleanup{output, errors, pid, reaped};
    int child_status = 0;
    std::string error;
    std::string diagnostics;
    std::string line;
    UniqueFile log;
    std::size_t log_bytes = 0;
    bool child_started = false;
    std::unique_ptr<GnssReceiver> gps;
    GnssFix gps_fix;
    bool gps_available = false;
    auto next_gps_poll = std::chrono::steady_clock::time_point{};
    AdsbPositionDecoder position_decoder;
    bool oversized = false;
    const auto close_fd = [](int& fd) { if (fd >= 0) { close(fd); fd = -1; } };
    const auto finish = [&] {
        close_fd(output[0]); close_fd(output[1]); close_fd(errors[0]); close_fd(errors[1]);
        if (log.stream) {
            errno = 0;
            if (std::fclose(log.stream.release()) != 0) {
                if (!error.empty()) error += "; ";
                error += std::string("Cannot close ADS-B log: ") + std::strerror(errno);
            }
            if (!child_started) error += remove_incomplete_file(log.path);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        status_.running = false;
        status_.complete = true;
        if (child_started && !log.path.empty()) status_.log_filename = log.path.filename().u8string();
        status_.error = error;
    };
    if (!log_home.empty()) {
        const auto logs = list_security_logs(log_home);
        if (!logs.error.empty()) { error = logs.error; finish(); return; }
        if (logs.entries.size() >= 256) { error = "Security log limit reached"; finish(); return; }
        const auto directory = security_log_directory(log_home, EvidenceSource::Adsb);
        error = ensure_confidential_directory(directory);
        if (!error.empty()) { error = "ADS-B log directory: " + error; finish(); return; }
        log = open_unique_file(directory, "adsb-receiver-", ".csv");
        if (!log.error.empty()) { error = log.error; finish(); return; }
        const std::string header = "receipt_unix_ms,aircraft_address,callsign,type,altitude_ft,"
                                   "reported_latitude,reported_longitude,emergency,squawk,"
                                   "receiver_latitude,receiver_longitude,receiver_fix_unix_ms,receiver_fix_mode\n";
        errno = 0;
        if (std::fwrite(header.data(), 1, header.size(), log.stream.get()) != header.size() ||
            std::fflush(log.stream.get()) != 0) {
            error = std::string("Cannot write ADS-B log: ") +
                    (errno ? std::strerror(errno) : "I/O error");
            finish(); return;
        }
        log_bytes = header.size();
    }
    if (!gps_device.empty()) {
        gps = GnssReceiver::open(gps_device, error);
        if (!gps) { error = "ADS-B GPS: " + error; finish(); return; }
        std::lock_guard<std::mutex> lock(mutex_);
        status_.gps_status = "GPS no current fix";
    }
    if (pipe2(output, O_CLOEXEC) != 0 || pipe2(errors, O_CLOEXEC) != 0) {
        error = std::string("Cannot create receiver pipes: ") + std::strerror(errno);
        finish(); return;
    }
    if (output[0] <= 2 || output[1] <= 2 || errors[0] <= 2 || errors[1] <= 2) {
        error = "Receiver standard descriptors unavailable";
        finish(); return;
    }
    for (const int fd : {output[0], errors[0]}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            error = std::string("Cannot configure receiver pipe: ") + std::strerror(errno);
            finish(); return;
        }
    }
    posix_spawn_file_actions_t actions;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    if (spawn_error) {
        error = std::string("Cannot configure receiver: ") + std::strerror(spawn_error);
        finish(); return;
    }
    const auto add = [&spawn_error](int result) { if (!spawn_error) spawn_error = result; };
    add(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    add(posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO));
    add(posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO));
    for (const int fd : {output[0], output[1], errors[0], errors[1]})
        add(posix_spawn_file_actions_addclose(&actions, fd));
    std::string allowed_errors = "-e";
    std::string zero = "0";
    std::string program = executable;
    char* args[] = {program.data(), allowed_errors.data(), zero.data(), nullptr};
    if (cancelled_) {
        posix_spawn_file_actions_destroy(&actions);
        finish(); return;
    }
    if (!spawn_error) spawn_error = posix_spawn(&pid, executable.c_str(), &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close_fd(output[1]); close_fd(errors[1]);
    if (spawn_error) {
        error = "Cannot start rtl_adsb: " + std::string(std::strerror(spawn_error));
        finish(); return;
    }
    child_started = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.running = true;
        if (log.stream) status_.log_filename = log.path.filename().u8string();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    bool stopping = false;
    auto stop_deadline = deadline;
    auto drain_deadline = deadline;
    while (!reaped || output[0] >= 0 || errors[0] >= 0) {
        const auto now = std::chrono::steady_clock::now();
        if (gps && now >= next_gps_poll && error.empty()) {
            std::string gps_error;
            if (!gps->poll(gps_fix, gps_available, gps_error)) {
                gps_available = false;
                error = "ADS-B GPS: " + gps_error;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.gps_status = gps_available &&
                    gnss_fix_is_fresh(gps_fix, std::chrono::system_clock::now())
                    ? "GPS fix" : "GPS no current fix";
            }
            next_gps_poll = now + std::chrono::milliseconds(200);
        }
        if (!reaped) {
            const auto result = waitpid(pid, &child_status, WNOHANG);
            if (result == pid) {
                reaped = true;
                drain_deadline = now + std::chrono::seconds(1);
            } else if (result < 0 && errno != EINTR) {
                error = std::string("Cannot wait for receiver: ") + std::strerror(errno);
                reaped = true;
                drain_deadline = now + std::chrono::seconds(1);
            }
        }
        if (reaped && now >= drain_deadline) {
            if (error.empty()) error = "Receiver output did not close";
            break;
        }
        if (!reaped && !stopping && (cancelled_ || now >= deadline || !error.empty())) {
            if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
                error = std::string("Cannot stop receiver: ") + std::strerror(errno);
            stopping = true;
            stop_deadline = now + std::chrono::seconds(2);
        } else if (!reaped && stopping && now >= stop_deadline) {
            if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
                error = std::string("Cannot terminate receiver: ") + std::strerror(errno);
            stop_deadline = now + std::chrono::seconds(2);
        }
        pollfd fds[2]{{output[0], POLLIN, 0}, {errors[0], POLLIN, 0}};
        const int ready = poll(fds, 2, 100);
        if (ready < 0 && errno != EINTR && error.empty())
            error = std::string("Receiver poll failed: ") + std::strerror(errno);
        for (int index = 0; index < 2; ++index) {
            if (!(fds[index].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buffer[2048];
            const auto count = read(fds[index].fd, buffer, sizeof(buffer));
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && error.empty())
                error = std::string("Receiver read failed: ") + std::strerror(errno);
            if (count == 0) {
                if (index == 0) close_fd(output[0]); else close_fd(errors[0]);
            }
            for (ssize_t i = 0; i < count; ++i) {
                const unsigned char ch = static_cast<unsigned char>(buffer[i]);
                if (index == 1) {
                    if (diagnostics.size() == 256) diagnostics.erase(0, 1);
                    diagnostics += ch >= 32 && ch <= 126 ? static_cast<char>(ch) : ' ';
                } else if (ch == '\n') {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    AdsbFrame frame;
                    if (!oversized && line.size() == 30 && line.front() == '*' && line.back() == ';')
                        frame = decode_adsb_frame(line.substr(1, 28));
                    else frame.error = "Invalid frame";
                    if (frame.error.empty())
                        frame = position_decoder.update(std::move(frame), std::chrono::steady_clock::now());
                    if (frame.error.empty() && log.stream && error.empty()) {
                        std::ostringstream row;
                        row.imbue(std::locale::classic());
                        const auto recorded_at = std::chrono::system_clock::now();
                        row << std::chrono::duration_cast<std::chrono::milliseconds>(
                            recorded_at.time_since_epoch()).count() << ','
                            << frame.aircraft_address << ',' << frame.callsign << ',' << frame.type_code << ',';
                        if (frame.altitude_available) row << frame.altitude_feet;
                        row << ',';
                        if (frame.position_available)
                            row << std::fixed << std::setprecision(7) << frame.latitude;
                        row << ',';
                        if (frame.position_available)
                            row << std::fixed << std::setprecision(7) << frame.longitude;
                        row << ',';
                        if (frame.emergency_status_available) row << static_cast<int>(frame.emergency_status);
                        row << ',' << frame.squawk << ',';
                        const bool gps_fresh = gps_available && gnss_fix_is_fresh(gps_fix, recorded_at);
                        if (gps_fresh) row << std::fixed << std::setprecision(7) << gps_fix.latitude;
                        row << ',';
                        if (gps_fresh) row << std::fixed << std::setprecision(7) << gps_fix.longitude;
                        row << ',';
                        if (gps_fresh) row << std::chrono::duration_cast<std::chrono::milliseconds>(
                            gps_fix.utc.time_since_epoch()).count();
                        row << ',';
                        if (gps_fresh) row << gps_fix.mode;
                        row << '\n';
                        const auto text = row.str();
                        constexpr std::size_t limit = 1024 * 1024;
                        if (text.size() > limit - log_bytes) error = "ADS-B log limit reached (1 MiB)";
                        else {
                            errno = 0;
                            if (std::fwrite(text.data(), 1, text.size(), log.stream.get()) != text.size() ||
                                std::fflush(log.stream.get()) != 0)
                                error = std::string("Cannot write ADS-B log: ") +
                                        (errno ? std::strerror(errno) : "I/O error");
                            else log_bytes += text.size();
                        }
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (frame.error.empty()) {
                        ++status_.valid_frames;
                        status_.latest = frame;
                        auto& recent = status_.recent_aircraft;
                        const auto found = std::find_if(recent.begin(), recent.end(),
                            [&](const AdsbFrame& item) {
                                return item.aircraft_address == frame.aircraft_address;
                            });
                        if (found != recent.end()) recent.erase(found);
                        else if (recent.size() == kAdsbRecentAircraftLimit) recent.pop_back();
                        recent.insert(recent.begin(), std::move(frame));
                    } else ++status_.rejected_frames;
                    line.clear();
                    oversized = false;
                } else if (!oversized) {
                    if (line.size() == 128) { line.clear(); oversized = true; }
                    else line += static_cast<char>(ch);
                }
            }
        }
    }
    if (!stopping && error.empty()) {
        error = "Receiver exited";
        if (!diagnostics.empty()) error += ": " + diagnostics;
    }
    finish();
#else
    (void)executable;
    (void)log_home;
    (void)gps_device;
#endif
}

} // namespace platform
