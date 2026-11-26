#include "lab_honeypot.h"

#include "unique_file.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <system_error>

#if !USE_DESKTOP
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace platform {

bool valid_lab_honeypot_address(const std::string& address) {
    unsigned octets[4]{};
    std::size_t position = 0;
    for (unsigned& octet : octets) {
        const auto start = position;
        while (position < address.size() && address[position] >= '0' && address[position] <= '9') {
            if (position - start == 3) return false;
            octet = octet * 10 + static_cast<unsigned>(address[position++] - '0');
        }
        if (position == start || octet > 255 ||
            (position - start > 1 && address[start] == '0')) return false;
        if (&octet != &octets[3]) {
            if (position >= address.size() || address[position++] != '.') return false;
        }
    }
    if (position != address.size()) return false;
    return octets[0] == 10 ||
           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
           (octets[0] == 192 && octets[1] == 168) ||
           (octets[0] == 169 && octets[1] == 254) ||
           (octets[0] == 127 && octets[1] == 0 && octets[2] == 0 && octets[3] == 1);
}

std::unique_ptr<LabHoneypot> LabHoneypot::start(const std::string& address,
                                                std::filesystem::path directory,
                                                std::string& error) {
    error.clear();
    if (!valid_lab_honeypot_address(address)) {
        error = "Enter a private or loopback IPv4 address";
        return nullptr;
    }
#if USE_DESKTOP
    (void)directory;
    error = "Lab listener unavailable in desktop simulator";
    return nullptr;
#else
    auto session = std::unique_ptr<LabHoneypot>(new LabHoneypot);
    try {
        session->thread_ = std::thread(&LabHoneypot::run, session.get(), address,
                                       std::move(directory));
    } catch (const std::system_error& failure) {
        error = "Cannot start listener: " + failure.code().message();
        return nullptr;
    }
    return session;
#endif
}

LabHoneypot::~LabHoneypot() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void LabHoneypot::stop() { cancelled_ = true; }

LabHoneypotStatus LabHoneypot::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void LabHoneypot::run(std::string address, std::filesystem::path directory) {
#if !USE_DESKTOP
    std::string error;
    int listener = -1;
    try {
        if (!cancelled_) {
            listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (listener < 0) error = std::string("Socket: ") + std::strerror(errno);
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(8088);
        if (error.empty() && !cancelled_ &&
            inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1)
            error = "Invalid IPv4 address";
        if (error.empty() && !cancelled_ &&
            bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
            error = std::string("Bind: ") + std::strerror(errno);
        UniqueFile log;
        bool header_written = false;
        bool log_created = false;
        if (error.empty() && !cancelled_) {
            error = ensure_private_directory(directory);
            if (error.empty()) {
                log = open_unique_file(directory, "lab-listener-", ".txt");
                error = log.error;
                log_created = static_cast<bool>(log.stream);
            }
        }
        if (error.empty() && !cancelled_) {
            if (std::fprintf(log.stream.get(),
                             "POSEIDON lab listener\nBind: %s:8088\n"
                             "UTC time is Unix seconds. Peer addresses are unverified.\n"
                             "No payload or credentials are collected.\n",
                             address.c_str()) < 0 || std::fflush(log.stream.get()) != 0)
                error = std::string("Write log: ") + std::strerror(errno);
            else
                header_written = true;
        }
        if (error.empty() && !cancelled_) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.file_name = log.path.filename().string();
        }
        if (error.empty() && !cancelled_ && listen(listener, 8) != 0)
            error = std::string("Listen: ") + std::strerror(errno);
        if (error.empty() && !cancelled_) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.listening = true;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        unsigned count = 0;
        while (error.empty() && !cancelled_ && count < 100 &&
               std::chrono::steady_clock::now() < deadline) {
            pollfd ready{listener, POLLIN, 0};
            const int polled = poll(&ready, 1, 200);
            if (polled < 0) {
                if (errno == EINTR) continue;
                error = std::string("Poll: ") + std::strerror(errno);
                break;
            }
            if (polled == 0) continue;
            if ((ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                error = "Listener socket failed";
                break;
            }
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            const int client = accept4(listener, reinterpret_cast<sockaddr*>(&peer), &length,
                                       SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (client < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                error = std::string("Accept: ") + std::strerror(errno);
                break;
            }
            close(client);
            char peer_address[INET_ADDRSTRLEN]{};
            if (!inet_ntop(AF_INET, &peer.sin_addr, peer_address, sizeof(peer_address))) {
                error = std::string("Peer address: ") + std::strerror(errno);
                break;
            }
            const auto time = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (std::fprintf(log.stream.get(), "%lld %s\n",
                             static_cast<long long>(time), peer_address) < 0 ||
                std::fflush(log.stream.get()) != 0) {
                error = std::string("Write log: ") + std::strerror(errno);
                break;
            }
            ++count;
            std::lock_guard<std::mutex> lock(mutex_);
            status_.connections = count;
            status_.last_peer = peer_address;
        }
        if (header_written && error.empty()) {
            const char* reason = cancelled_ ? "cancelled" :
                                 count >= 100 ? "connection limit" : "time limit";
            if (std::fprintf(log.stream.get(), "Stopped: %s\n", reason) < 0 ||
                std::fflush(log.stream.get()) != 0)
                error = std::string("Write log: ") + std::strerror(errno);
        }
        if (log.stream && std::fclose(log.stream.release()) != 0 && error.empty())
            error = std::string("Close log: ") + std::strerror(errno);
        if (log_created && !header_written)
            error += remove_incomplete_file(log.path);
    } catch (const std::exception& failure) {
        error = std::string("Listener failed: ") + failure.what();
    }
    if (listener >= 0) close(listener);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.listening = false;
        status_.complete = true;
        status_.error = error;
    }
#else
    (void)address;
    (void)directory;
#endif
}

} // namespace platform
