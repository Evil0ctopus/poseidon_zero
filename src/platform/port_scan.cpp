#include "port_scan.h"
#include "unique_file.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#if !USE_DESKTOP
#include <cerrno>
#include <fcntl.h>
#include <gio/gio.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

bool parse_number(const std::string& value, unsigned& result) {
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool usable_address(int family, const unsigned char* address) {
    if (family == AF_INET) {
        return address[0] != 0 && address[0] < 224 &&
               !(address[0] == 255 && address[1] == 255 &&
                 address[2] == 255 && address[3] == 255);
    }
    const bool mapped = std::all_of(address, address + 10, [](unsigned char ch) { return ch == 0; }) &&
                        address[10] == 0xff && address[11] == 0xff;
    return !mapped && address[0] != 0xff &&
           std::any_of(address, address + 16, [](unsigned char ch) { return ch != 0; });
}

PortTargets numeric_targets(const std::string& input, bool& numeric) {
    PortTargets result;
    const auto slash = input.find('/');
    const auto address_text = input.substr(0, slash);
    std::array<unsigned char, 16> bytes{};
    int family = AF_INET;
    if (inet_pton(AF_INET, address_text.c_str(), bytes.data()) != 1) {
        family = AF_INET6;
        if (inet_pton(AF_INET6, address_text.c_str(), bytes.data()) != 1) {
            numeric = false;
            if (slash != std::string::npos) result.error = "CIDR requires an IP address";
            return result;
        }
    }
    numeric = true;
    const unsigned bits = family == AF_INET ? 32 : 128;
    unsigned prefix = bits;
    if (slash != std::string::npos &&
        (!parse_number(input.substr(slash + 1), prefix) || prefix > bits)) {
        result.error = "Invalid CIDR prefix";
        return result;
    }
    if (bits - prefix > 8) {
        result.error = "Subnet exceeds 256 addresses";
        return result;
    }
    if (!usable_address(family, bytes.data())) {
        result.error = "Target must be a unicast address";
        return result;
    }
    const auto byte_count = bits / 8;
    for (unsigned bit = prefix; bit < bits; ++bit)
        bytes[bit / 8] &= static_cast<unsigned char>(~(1U << (7 - bit % 8)));
    const unsigned count = 1U << (bits - prefix);
    const unsigned first = family == AF_INET && count > 2 ? 1 : 0;
    const unsigned end = family == AF_INET && count > 2 ? count - 1 : count;
    for (unsigned index = first; index < end; ++index) {
        auto current = bytes;
        current[byte_count - 1] = static_cast<unsigned char>(current[byte_count - 1] + index);
        if (!usable_address(family, current.data())) {
            result.addresses.clear();
            result.error = "Subnet includes non-unicast addresses";
            return result;
        }
        char text[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(family, current.data(), text, sizeof(text))) {
            result.addresses.clear();
            result.error = "Cannot format target address";
            return result;
        }
        result.addresses.emplace_back(text);
    }
    return result;
}

#if !USE_DESKTOP
struct ResolveState {
    GMainLoop* loop;
    GCancellable* cancellable;
    const std::atomic_bool* cancelled;
    gint64 deadline;
    bool timed_out{false};
    GList* addresses{nullptr};
    GError* error{nullptr};
};

void resolve_done(GObject* resolver, GAsyncResult* result, gpointer data) {
    auto& state = *static_cast<ResolveState*>(data);
    state.addresses = g_resolver_lookup_by_name_finish(G_RESOLVER(resolver), result, &state.error);
    g_main_loop_quit(state.loop);
}

gboolean check_resolver(gpointer data) {
    auto& state = *static_cast<ResolveState*>(data);
    if (*state.cancelled || g_get_monotonic_time() >= state.deadline) {
        state.timed_out = !*state.cancelled;
        g_cancellable_cancel(state.cancellable);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
#endif

} // namespace

PortTargets numeric_port_targets(const std::string& input) {
    bool numeric = false;
    auto result = numeric_targets(input, numeric);
    if (!numeric && result.error.empty()) result.error = "Enter a numeric IP or CIDR";
    return result;
}

PortList parse_port_list(const std::string& input) {
    PortList result;
    if (input.empty() || input.size() > 4096) {
        result.error = "Enter ports or ranges";
        return result;
    }
    std::vector<bool> selected(65536, false);
    std::size_t start = 0;
    while (start < input.size()) {
        const auto comma = input.find(',', start);
        const auto token = input.substr(start, comma == std::string::npos ? comma : comma - start);
        const auto dash = token.find('-');
        unsigned first = 0, last = 0;
        if (!parse_number(token.substr(0, dash), first) ||
            (dash != std::string::npos && !parse_number(token.substr(dash + 1), last))) {
            result.error = "Invalid port or range";
            return result;
        }
        if (dash == std::string::npos) last = first;
        if (first == 0 || last > 65535 || first > last) {
            result.error = "Ports must be 1-65535 in ascending ranges";
            return result;
        }
        for (unsigned port = first; port <= last; ++port) selected[port] = true;
        if (comma == std::string::npos) break;
        start = comma + 1;
        if (start == input.size()) {
            result.error = "Invalid port or range";
            return result;
        }
    }
    for (unsigned port = 1; port <= 65535; ++port)
        if (selected[port]) result.ports.push_back(static_cast<uint16_t>(port));
    return result;
}

PortList common_ports() {
    return parse_port_list("21-23,25,53,67-69,80,110,123,135,137-139,143,161,389,443,445,465,"
                           "500,587,631,636,993,995,1433,1521,1883,1900,2049,3000,3306,3389,"
                           "5000,5353,5432,5900,6379,8000,8080,8443,8883");
}

std::string format_port_list(const std::vector<uint16_t>& ports) {
    std::string text;
    for (std::size_t i = 0; i < ports.size();) {
        std::size_t last = i;
        while (last + 1 < ports.size() &&
               static_cast<unsigned>(ports[last]) + 1 == ports[last + 1]) ++last;
        if (!text.empty()) text += ',';
        text += std::to_string(ports[i]);
        if (last != i) text += '-' + std::to_string(ports[last]);
        i = last + 1;
    }
    return text;
}

PortTargets resolve_port_targets(const std::string& input, const std::atomic_bool& cancelled) {
    if (cancelled) return {};
    if (input.empty() || input.size() > 253) return {{}, "Invalid target"};
    bool numeric = false;
    auto result = numeric_targets(input, numeric);
    if (numeric || !result.error.empty()) return result;
    if (input.find(':') != std::string::npos ||
        std::all_of(input.begin(), input.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || ch == '.';
        }) ||
        !std::all_of(input.begin(), input.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
        })) {
        return {{}, "Invalid hostname or IP address"};
    }
#if USE_DESKTOP
    return {{}, "Hostname resolution requires a supported device"};
#else
    auto* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    auto* loop = g_main_loop_new(context, FALSE);
    auto* cancellable = g_cancellable_new();
    auto* resolver = g_resolver_get_default();
    ResolveState state{loop, cancellable, &cancelled,
                       g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND};
    auto* timer = g_timeout_source_new(25);
    g_source_set_callback(timer, check_resolver, &state, nullptr);
    g_source_attach(timer, context);
    g_resolver_lookup_by_name_async(resolver, input.c_str(), cancellable, resolve_done, &state);
    g_main_loop_run(loop);
    g_source_destroy(timer);
    g_source_unref(timer);
    g_object_unref(resolver);
    g_object_unref(cancellable);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    if (cancelled) {
        if (state.addresses) g_resolver_free_addresses(state.addresses);
        if (state.error) g_error_free(state.error);
        return {};
    }
    if (state.error) {
        result.error = state.timed_out ? "DNS lookup timed out" : "DNS: " + std::string(state.error->message);
        g_error_free(state.error);
        return result;
    }
    for (auto* item = state.addresses; item; item = item->next) {
        auto* address = G_INET_ADDRESS(item->data);
        const auto family = g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4
                                ? AF_INET : AF_INET6;
        if (!usable_address(family, g_inet_address_to_bytes(address))) {
            result.error = "DNS returned a non-unicast address";
            break;
        }
        gchar* text = g_inet_address_to_string(address);
        if (std::find(result.addresses.begin(), result.addresses.end(), text) == result.addresses.end())
            result.addresses.emplace_back(text);
        g_free(text);
        if (result.addresses.size() > 256) {
            result.error = "DNS returned more than 256 addresses";
            break;
        }
    }
    g_resolver_free_addresses(state.addresses);
    if (!result.error.empty()) result.addresses.clear();
    else if (result.addresses.empty()) result.error = "DNS returned no addresses";
    return result;
#endif
}

const char* port_state_name(PortState state) {
    switch (state) {
        case PortState::Open: return "open";
        case PortState::Closed: return "closed";
        case PortState::Filtered: return "filtered";
        case PortState::OpenOrFiltered: return "open or filtered";
        case PortState::Unreachable: return "unreachable";
        case PortState::Error: return "error";
    }
    return "unknown";
}

namespace {

constexpr std::size_t kMaxEntries = 2048;

void record_result(PortScanResult& result, PortResult entry) {
    switch (entry.state) {
        case PortState::Open: ++result.open; break;
        case PortState::Closed: ++result.closed; break;
        case PortState::Filtered: ++result.filtered; break;
        case PortState::OpenOrFiltered: ++result.open_or_filtered; break;
        case PortState::Unreachable: ++result.unreachable; break;
        case PortState::Error: ++result.errors; break;
    }
    if (result.entries.size() < kMaxEntries) {
        result.entries.push_back(std::move(entry));
    } else {
        result.truncated = true;
        if (entry.state == PortState::Open) {
            auto replace = std::find_if(result.entries.begin(), result.entries.end(),
                                        [](const PortResult& item) { return item.state != PortState::Open; });
            if (replace != result.entries.end()) *replace = std::move(entry);
        }
    }
}

#if !USE_DESKTOP
using Clock = std::chrono::steady_clock;

struct Probe {
    int fd{-1};
    PortResult result;
    Clock::time_point deadline;

    Probe() = default;
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    Probe(Probe&& other) noexcept
        : fd(std::exchange(other.fd, -1)), result(std::move(other.result)),
          deadline(other.deadline) {}
    Probe& operator=(Probe&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) close(fd);
            fd = std::exchange(other.fd, -1);
            result = std::move(other.result);
            deadline = other.deadline;
        }
        return *this;
    }
    ~Probe() { if (fd >= 0) close(fd); }
};

PortState error_state(int error, PortProtocol protocol) {
    if (error == ECONNREFUSED) return PortState::Closed;
    if (error == ETIMEDOUT) return protocol == PortProtocol::Tcp
                                     ? PortState::Filtered : PortState::OpenOrFiltered;
    if (error == EHOSTUNREACH || error == ENETUNREACH) return PortState::Unreachable;
    return PortState::Error;
}

bool make_address(const std::string& text, uint16_t port, sockaddr_storage& storage,
                  socklen_t& length) {
    auto* ipv4 = reinterpret_cast<sockaddr_in*>(&storage);
    if (inet_pton(AF_INET, text.c_str(), &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(port);
        length = sizeof(sockaddr_in);
        return true;
    }
    auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&storage);
    if (inet_pton(AF_INET6, text.c_str(), &ipv6->sin6_addr) == 1) {
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(port);
        length = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}
#endif

} // namespace

PortScanResult scan_ports(const PortTargets& targets, const std::vector<uint16_t>& ports,
                          bool tcp, bool udp, PortScanProgress& progress) {
    PortScanResult result;
    progress.completed = 0;
    progress.total = 0;
    if (!targets.error.empty() || targets.addresses.empty() || targets.addresses.size() > 256 ||
        ports.empty() || ports.size() > 65535 || (!tcp && !udp) ||
        std::find(ports.begin(), ports.end(), 0) != ports.end()) {
        result.error = "Invalid scan configuration";
        return result;
    }
    result.targets = targets.addresses;
    result.ports = ports;
    result.tcp = tcp;
    result.udp = udp;
#if USE_DESKTOP
    result.error = "Port scanning requires a supported device";
    return result;
#else
    for (const auto& address : targets.addresses) {
        bool numeric = false;
        auto validated = numeric_targets(address, numeric);
        if (!numeric || !validated.error.empty() || validated.addresses.size() != 1) {
            result.error = "Invalid scan target";
            return result;
        }
    }
    const uint64_t per_address = static_cast<uint64_t>(ports.size()) * (tcp && udp ? 2 : 1);
    const uint64_t total = per_address * targets.addresses.size();
    result.total = total;
    progress.total = total;
    std::vector<Probe> active;
    active.reserve(16);
    uint64_t next = 0;
    auto next_launch = Clock::now();
    const auto finish = [&](Probe& probe, PortState state) {
        close(probe.fd);
        probe.fd = -1;
        probe.result.state = state;
        record_result(result, std::move(probe.result));
        ++progress.completed;
    };
    while ((!progress.cancelled && next < total) || !active.empty()) {
        if (progress.cancelled) break;
        const auto now = Clock::now();
        if (next < total && active.size() < 16 && now >= next_launch) {
            const auto address_index = static_cast<std::size_t>(next / per_address);
            const auto offset = next % per_address;
            const auto port_index = static_cast<std::size_t>(offset / (tcp && udp ? 2 : 1));
            const auto protocol = tcp && (!udp || offset % 2 == 0) ? PortProtocol::Tcp : PortProtocol::Udp;
            Probe probe;
            probe.result = {targets.addresses[address_index], ports[port_index], protocol, PortState::Error};
            sockaddr_storage address{};
            socklen_t length = 0;
            if (!make_address(probe.result.address, probe.result.port, address, length)) {
                result.error = "Cannot parse scan target";
                break;
            }
            probe.fd = socket(address.ss_family,
                              (protocol == PortProtocol::Tcp ? SOCK_STREAM : SOCK_DGRAM) |
                                  SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (probe.fd < 0) {
                result.error = std::string("Cannot create scan socket: ") + std::strerror(errno);
                break;
            }
            const int connected = connect(probe.fd, reinterpret_cast<sockaddr*>(&address), length);
            if (connected < 0 && errno != EINPROGRESS) {
                finish(probe, error_state(errno, protocol));
            } else if (protocol == PortProtocol::Tcp && connected == 0) {
                finish(probe, PortState::Open);
            } else if (protocol == PortProtocol::Udp && send(probe.fd, "", 0, 0) < 0) {
                finish(probe, error_state(errno, protocol));
            } else {
                probe.deadline = Clock::now() + std::chrono::seconds(1);
                active.push_back(std::move(probe));
            }
            ++next;
            next_launch = Clock::now() + std::chrono::milliseconds(20);
        }
        std::vector<pollfd> descriptors;
        descriptors.reserve(active.size());
        for (const auto& probe : active)
            descriptors.push_back({probe.fd, static_cast<short>(
                probe.result.protocol == PortProtocol::Tcp ? POLLOUT : POLLIN), 0});
        const int ready = poll(descriptors.data(), descriptors.size(), 20);
        if (ready < 0 && errno != EINTR) {
            result.error = std::string("Scan polling failed: ") + std::strerror(errno);
            break;
        }
        for (std::size_t i = active.size(); i-- > 0;) {
            auto& probe = active[i];
            const auto events = descriptors[i].revents;
            bool done = false;
            PortState state = PortState::Error;
            if (events & (POLLERR | POLLHUP | POLLNVAL)) {
                int error = 0;
                socklen_t length = sizeof(error);
                if (getsockopt(probe.fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                    error = errno;
                state = error ? error_state(error, probe.result.protocol)
                              : (probe.result.protocol == PortProtocol::Tcp && (events & POLLOUT)
                                     ? PortState::Open : PortState::Error);
                done = true;
            } else if (events & (POLLIN | POLLOUT)) {
                if (probe.result.protocol == PortProtocol::Tcp) {
                    int error = 0;
                    socklen_t length = sizeof(error);
                    if (getsockopt(probe.fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                        error = errno;
                    state = error ? error_state(error, probe.result.protocol) : PortState::Open;
                    done = true;
                } else {
                    char buffer[1];
                    const auto received = recv(probe.fd, buffer, sizeof(buffer), 0);
                    if (received >= 0) {
                        state = PortState::Open;
                        done = true;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        state = error_state(errno, probe.result.protocol);
                        done = true;
                    }
                }
            }
            if (!done && Clock::now() >= probe.deadline) {
                state = probe.result.protocol == PortProtocol::Tcp
                            ? PortState::Filtered : PortState::OpenOrFiltered;
                done = true;
            }
            if (done) {
                finish(probe, state);
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
    result.cancelled = progress.cancelled;
    result.completed = progress.completed;
    return result;
#endif
}

PortReportResult save_port_report(const std::filesystem::path& directory,
                                 const PortScanResult& result) {
    const auto formatted_ports = format_port_list(result.ports);
    const auto folder_error = ensure_private_directory(directory);
    if (!folder_error.empty()) return {{}, "Report folder: " + folder_error};
    auto output = open_unique_file(directory, "poseidon-port-scan-", ".txt");
    if (!output.error.empty()) return {{}, output.error};
    FILE* file = output.stream.get();
    bool ok = std::fprintf(file, "POSEIDON port scan\n%s\n%s\n",
                           !result.error.empty() ? "Failed (partial results)" :
                           result.cancelled ? "Cancelled (partial results)" : "Completed",
                           result.truncated ? "Results truncated" : "Results complete") >= 0;
    if (ok && !result.error.empty()) ok = std::fprintf(file, "Error: %s\n", result.error.c_str()) >= 0;
    if (ok) ok = std::fprintf(file, "Protocols: %s%s\nCompleted: %llu/%llu\nTargets:\n",
                              result.tcp ? "TCP" : "", result.udp ? (result.tcp ? ", UDP" : "UDP") : "",
                              static_cast<unsigned long long>(result.completed),
                              static_cast<unsigned long long>(result.total)) >= 0;
    for (const auto& target : result.targets) {
        if (ok) ok = std::fprintf(file, "  %s\n", target.c_str()) >= 0;
    }
    if (ok) ok = std::fprintf(file, "Ports: %s\n", formatted_ports.c_str()) >= 0;
    if (ok) ok = std::fprintf(file, "open=%llu closed=%llu filtered=%llu open_or_filtered=%llu "
                                   "unreachable=%llu errors=%llu\n",
                              static_cast<unsigned long long>(result.open),
                              static_cast<unsigned long long>(result.closed),
                              static_cast<unsigned long long>(result.filtered),
                              static_cast<unsigned long long>(result.open_or_filtered),
                              static_cast<unsigned long long>(result.unreachable),
                              static_cast<unsigned long long>(result.errors)) >= 0;
    for (const auto& entry : result.entries) {
        if (!ok) break;
        ok = std::fprintf(file, "%s %s %u %s\n", entry.address.c_str(),
                          entry.protocol == PortProtocol::Tcp ? "tcp" : "udp",
                          static_cast<unsigned>(entry.port), port_state_name(entry.state)) >= 0;
    }
    if (std::fclose(output.stream.release()) != 0) ok = false;
    if (!ok) return {{}, "Could not write report" + remove_incomplete_file(output.path)};
    return {output.path, {}};
}

} // namespace platform
