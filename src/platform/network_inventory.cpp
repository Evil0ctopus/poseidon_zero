#include "network_inventory.h"
#include "unique_file.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace platform {
namespace {

std::string report_field(const std::string& value) {
    std::string result;
    for (unsigned char ch : value) {
        if (result.size() == 80) break;
        result += ch >= 32 && ch <= 126 ? static_cast<char>(ch) : '?';
    }
    return result;
}

bool usable_mac(const std::string& value) {
    if (value.size() != 17 || value == "00:00:00:00:00:00") return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (i % 3 == 2) {
            if (ch != ':') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool equal_mac(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        const auto lower = [](char ch) {
            return ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch;
        };
        return lower(a) == lower(b);
    });
}

constexpr const char* kPrefix = "poseidon-host-discovery-";
constexpr const char* kSuffix = ".txt";
constexpr const char* kBaselineFile = "inventory-baseline";

bool valid_filename(const std::string& name) {
    return is_unique_filename(name, kPrefix, kSuffix);
}

std::string token(const std::string& value) {
    std::string safe = report_field(value);
    for (char& ch : safe) if (ch == ' ') ch = '_';
    return safe.empty() ? "-" : safe;
}

bool single_numeric_address(const std::string& address) {
    if (address.find('/') != std::string::npos) return false;
    const auto parsed = numeric_port_targets(address);
    return parsed.error.empty() && parsed.addresses.size() == 1 &&
           parsed.addresses.front() == address;
}

} // namespace

ArpComparison compare_arp_entry(const ArpResult& baseline, const ArpEntry& selected,
                                const ArpResult& current) {
    ArpComparison result;
    if (!baseline.error.empty() || !current.error.empty()) {
        result.detail = !baseline.error.empty() ? baseline.error : current.error;
        return result;
    }
    if (baseline.truncated || current.truncated) {
        result.detail = "ARP snapshot truncated";
        return result;
    }
    if (selected.address.empty() || selected.interface_name.empty() || !usable_mac(selected.hardware_address)) {
        result.detail = "Original MAC unavailable";
        return result;
    }
    const auto find_one = [&](const ArpResult& snapshot, const ArpEntry*& match) {
        for (const auto& entry : snapshot.entries) {
            if (entry.address != selected.address || entry.interface_name != selected.interface_name) continue;
            if (match) return false;
            match = &entry;
        }
        return true;
    };
    const ArpEntry* before = nullptr;
    const ArpEntry* after = nullptr;
    if (!find_one(baseline, before) || !find_one(current, after)) {
        result.detail = "Ambiguous ARP entries";
    } else if (!before || !equal_mac(before->hardware_address, selected.hardware_address)) {
        result.detail = "Original entry unavailable";
    } else if (!after || !usable_mac(after->hardware_address)) {
        result.detail = "Current MAC unavailable";
    } else {
        result.current_mac = after->hardware_address;
        result.state = equal_mac(before->hardware_address, after->hardware_address)
            ? ArpComparisonState::Same : ArpComparisonState::Changed;
    }
    return result;
}

std::vector<std::string> inventory_port_guidance(const InventoryHost& host) {
    struct Guidance { uint16_t port; const char* text; };
    static constexpr Guidance hints[] = {
        {21, "TCP 21: If FTP, avoid plaintext credentials; restrict access."},
        {23, "TCP 23: If Telnet, avoid plaintext credentials; restrict access."},
        {80, "TCP 80: If HTTP, protect sensitive traffic with HTTPS."},
        {445, "TCP 445: If SMB, restrict access and disable obsolete versions."},
        {3389, "TCP 3389: If remote desktop, restrict access and require strong authentication."},
        {5900, "TCP 5900: If VNC, restrict access and protect authentication and traffic."}
    };
    std::vector<std::string> result;
    for (const auto& hint : hints) {
        if (std::find(host.open_ports.begin(), host.open_ports.end(), hint.port) != host.open_ports.end())
            result.emplace_back(hint.text);
    }
    return result;
}

NetworkInventory make_network_inventory(PortScanResult scan, const ArpResult& arp) {
    NetworkInventory inventory;
    inventory.scan = std::move(scan);
    inventory.arp_warning = arp.error;
    if (arp.truncated) {
        if (!inventory.arp_warning.empty()) inventory.arp_warning += "; ";
        inventory.arp_warning += "ARP cache truncated";
    }
    for (const auto& target : inventory.scan.targets) {
        InventoryHost host;
        host.address = target;
        bool seen = false;
        for (const auto& entry : inventory.scan.entries) {
            if (entry.address != target || entry.protocol != PortProtocol::Tcp) continue;
            if (entry.state == PortState::Open || entry.state == PortState::Closed) seen = true;
            if (entry.state == PortState::Open) host.open_ports.push_back(entry.port);
        }
        if (!seen) continue;
        const ArpEntry* match = nullptr;
        bool ambiguous = false;
        for (const auto& entry : arp.entries) {
            if (entry.address != target || !usable_mac(entry.hardware_address)) continue;
            if (match) { ambiguous = true; break; }
            match = &entry;
        }
        if (match && !ambiguous) {
            host.cached_mac = match->hardware_address;
            host.interface_name = match->interface_name;
        }
        inventory.hosts.push_back(std::move(host));
    }
    return inventory;
}

InventoryPolicyReview review_inventory_policy(const NetworkInventory& inventory,
                                               const InventoryHost& host,
                                               const std::vector<uint16_t>& restricted_ports) {
    InventoryPolicyReview review;
    for (const auto port : restricted_ports) {
        if (std::find(host.open_ports.begin(), host.open_ports.end(), port) != host.open_ports.end())
            review.observed_open.push_back(port);
        if (std::find(inventory.scan.ports.begin(), inventory.scan.ports.end(), port) ==
            inventory.scan.ports.end())
            review.not_in_scan.push_back(port);
    }
    return review;
}

PortReportResult save_network_inventory(const std::filesystem::path& directory,
                                        const NetworkInventory& inventory) {
    const auto folder_error = ensure_private_directory(directory);
    if (!folder_error.empty()) return {{}, "Report folder: " + folder_error};
    const auto history = list_network_inventories(directory);
    if (!history.error.empty()) return {{}, history.error};
    if (history.names.size() >= 256)
        return {{}, "Report limit reached (256); delete a report first"};
    auto output = open_unique_file(directory, kPrefix, kSuffix);
    if (!output.error.empty()) return {{}, output.error};
    FILE* file = output.stream.get();
    const auto& scan = inventory.scan;
    bool ok = std::fprintf(file,
        "POSEIDON host discovery\n%s\nCompleted probes: %llu/%llu\n"
        "TCP ports: %s\nResponses indicate reachability, not verified identity.\n"
        "Unseen targets are unknown; firewalls and sleeping hosts may not respond.\n"
        "MAC/interface values are unverified cached ARP associations, not topology.\n"
        "Targets:\n",
        !scan.error.empty() ? "Failed (partial results)" :
        scan.cancelled ? "Cancelled (partial results)" : "Completed",
        static_cast<unsigned long long>(scan.completed),
        static_cast<unsigned long long>(scan.total),
        format_port_list(scan.ports).c_str()) >= 0;
    if (scan.truncated && ok) ok = std::fprintf(file, "WARNING: probe results truncated\n") >= 0;
    if (!scan.error.empty() && ok)
        ok = std::fprintf(file, "Error: %s\n", report_field(scan.error).c_str()) >= 0;
    if (!inventory.arp_warning.empty() && ok)
        ok = std::fprintf(file, "ARP warning: %s\n", report_field(inventory.arp_warning).c_str()) >= 0;
    if (!inventory.comparison_note.empty() && ok)
        ok = std::fprintf(file, "Baseline comparison: %s\n",
                          report_field(inventory.comparison_note).c_str()) >= 0;
    if (!inventory.comparison_baseline.empty() && ok)
        ok = std::fprintf(file, "Baseline report: %s\n",
                          report_field(inventory.comparison_baseline).c_str()) >= 0;
    for (const auto& target : scan.targets) {
        if (ok) ok = std::fprintf(file, "  %s\n", report_field(target).c_str()) >= 0;
    }
    if (ok) ok = std::fprintf(file,
        "Observation map (network paths and physical links unknown):\n"
        "  [this scanner]\n  ? [network path unknown]\n") >= 0;
    for (const auto& host : inventory.hosts) {
        if (ok) ok = std::fprintf(file, "    ? %s%s\n", report_field(host.address).c_str(),
                                  host.responded ? "" : " (not observed responding)") >= 0;
    }
    const auto responding = std::count_if(inventory.hosts.begin(), inventory.hosts.end(),
                                          [](const InventoryHost& host) { return host.responded; });
    if (ok) ok = std::fprintf(file, "Responding addresses: %zu\n",
                              static_cast<std::size_t>(responding)) >= 0;
    for (const auto& host : inventory.hosts) {
        if (!ok) break;
        const auto ports = format_port_list(host.open_ports);
        ok = std::fprintf(file, "%s%s  cached MAC: %s  interface: %s  open TCP: %s\n",
                          report_field(host.address).c_str(),
                          host.responded ? "" : " (not observed responding)",
                          host.cached_mac.empty() ? "unknown" : report_field(host.cached_mac).c_str(),
                          host.interface_name.empty() ? "unknown" : report_field(host.interface_name).c_str(),
                          ports.empty() ? "none observed" : ports.c_str()) >= 0;
        for (const auto& service : host.services) {
            if (ok) ok = std::fprintf(file, "  Nmap TCP %u: %s %s (unverified)\n",
                                      static_cast<unsigned>(service.port),
                                      report_field(service.service).c_str(),
                                      report_field(service.version).c_str()) >= 0;
        }
        if (ok && !host.os_guess.empty())
            ok = std::fprintf(file, "  Nmap OS guess: %s (unverified)\n",
                              report_field(host.os_guess).c_str()) >= 0;
        if (ok && !host.label.empty())
            ok = std::fprintf(file, "  User label: %s\n", report_field(host.label).c_str()) >= 0;
        if (ok && !host.update_note.empty())
            ok = std::fprintf(file, "  User update note: %s\n", report_field(host.update_note).c_str()) >= 0;
        if (ok && !host.update_due.empty())
            ok = std::fprintf(file, "  User update reminder date: %s\n", host.update_due.c_str()) >= 0;
        if (ok && host.router_listed)
            ok = std::fprintf(file, "  Imported router record (unverified): %s %s\n",
                              report_field(host.router_name).c_str(),
                              report_field(host.router_mac).c_str()) >= 0;
        if (ok && inventory_mac_conflict(host))
            ok = std::fprintf(file, "  WARNING: imported MAC differs from cached ARP\n") >= 0;
    }
    if (ok) ok = std::fprintf(file, "@inventory 1 %llu %llu %d %d %d\n",
                              static_cast<unsigned long long>(scan.completed),
                              static_cast<unsigned long long>(scan.total),
                              scan.cancelled ? 1 : 0, scan.error.empty() ? 0 : 1,
                              scan.truncated ? 1 : 0) >= 0;
    if (ok) ok = std::fprintf(file, "@ports %s\n", format_port_list(scan.ports).c_str()) >= 0;
    if (!inventory.comparison_note.empty()) {
        std::ostringstream line;
        line << "@comparison " << std::quoted(report_field(inventory.comparison_note)) << ' '
             << std::quoted(report_field(inventory.comparison_baseline)) << '\n';
        if (ok) ok = std::fputs(line.str().c_str(), file) >= 0;
    }
    if (!inventory.arp_warning.empty()) {
        std::ostringstream line;
        line << "@arp " << std::quoted(report_field(inventory.arp_warning)) << '\n';
        if (ok) ok = std::fputs(line.str().c_str(), file) >= 0;
    }
    for (const auto& target : scan.targets) {
        if (ok) ok = std::fprintf(file, "@target %s\n", token(target).c_str()) >= 0;
    }
    for (const auto& host : inventory.hosts) {
        if (ok) ok = std::fprintf(file, "@host %s %s %s %s %d\n", token(host.address).c_str(),
                                  token(host.cached_mac).c_str(), token(host.interface_name).c_str(),
                                  host.open_ports.empty() ? "-" :
                                      format_port_list(host.open_ports).c_str(),
                                  host.responded ? 1 : 0) >= 0;
        for (const auto& service : host.services) {
            std::ostringstream line;
            line << "@service " << token(host.address) << ' ' << service.port << ' '
                 << std::quoted(report_field(service.service)) << ' '
                 << std::quoted(report_field(service.version)) << '\n';
            if (ok) ok = std::fputs(line.str().c_str(), file) >= 0;
        }
        if (!host.os_guess.empty()) {
            std::ostringstream line;
            line << "@os " << token(host.address) << ' ' << std::quoted(report_field(host.os_guess)) << '\n';
            if (ok) ok = std::fputs(line.str().c_str(), file) >= 0;
        }
        if (!host.label.empty() || !host.update_note.empty() || !host.update_due.empty()) {
            std::ostringstream line;
            line << "@notes " << token(host.address) << ' ' << std::quoted(report_field(host.label))
                 << ' ' << std::quoted(report_field(host.update_note)) << ' '
                 << std::quoted(host.update_due) << '\n';
            if (ok) ok = std::fputs(line.str().c_str(), file) >= 0;
        }
        if (host.router_listed) {
            std::ostringstream line;
            line << "@router " << token(host.address) << ' ' << std::quoted(report_field(host.router_name))
                 << ' ' << std::quoted(report_field(host.router_mac)) << '\n';
            if (ok) ok = std::fputs(line.str().c_str(), file) >= 0;
        }
    }
    const auto length = std::ftell(file);
    const bool oversized = length > 1024 * 1024;
    if (length < 0 || oversized) ok = false;
    if (std::fclose(output.stream.release()) != 0) ok = false;
    if (!ok) return {{}, std::string(oversized ? "Report exceeds 1 MiB" : "Could not write report") +
                          remove_incomplete_file(output.path)};
    return {output.path, {}};
}

InventoryHistory list_network_inventories(const std::filesystem::path& directory) {
    InventoryHistory result;
    result.error = ensure_private_directory(directory);
    if (!result.error.empty()) return result;
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(directory, error); !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename().u8string();
        if (!valid_filename(name)) continue;
        const auto status = it->symlink_status(error);
        if (error) break;
        if (!std::filesystem::is_regular_file(status)) continue;
        if (result.names.size() == 256) {
            result.truncated = true;
            break;
        }
        result.names.push_back(name);
    }
    if (error) {
        result.names.clear();
        result.truncated = false;
        result.error = "Cannot list reports: " + error.message();
    } else {
        std::sort(result.names.begin(), result.names.end(), std::greater<std::string>());
    }
    return result;
}

NetworkInventory load_network_inventory(const std::filesystem::path& directory,
                                        const std::string& name) {
    NetworkInventory result;
    if (!valid_filename(name)) {
        result.scan.error = "Invalid report name";
        return result;
    }
    result.scan.error = ensure_private_directory(directory);
    if (!result.scan.error.empty()) return result;
    const auto path = directory / std::filesystem::u8path(name);
    std::string content;
#if !defined(_WIN32)
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        result.scan.error = std::string("Cannot open report: ") + std::strerror(errno);
        return result;
    }
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size > 1024 * 1024) {
        close(fd);
        result.scan.error = "Report must be a regular file under 1 MiB";
        return result;
    }
    char buffer[4096];
    for (;;) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            result.scan.error = std::string("Cannot read report: ") + std::strerror(errno);
            close(fd);
            return result;
        }
        if (!count) break;
        content.append(buffer, static_cast<std::size_t>(count));
        if (content.size() > 1024 * 1024) {
            close(fd);
            result.scan.error = "Report exceeds 1 MiB";
            return result;
        }
    }
    close(fd);
#else
    std::error_code fs_error;
    const auto status = std::filesystem::symlink_status(path, fs_error);
    if (fs_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::file_size(path, fs_error) > 1024 * 1024 || fs_error) {
        result.scan.error = "Report must be a regular file under 1 MiB";
        return result;
    }
    std::ifstream file_input(path, std::ios::binary);
    if (!file_input) {
        result.scan.error = "Cannot open report";
        return result;
    }
    char buffer[4096];
    while (file_input.read(buffer, sizeof(buffer)) || file_input.gcount()) {
        content.append(buffer, static_cast<std::size_t>(file_input.gcount()));
        if (content.size() > 1024 * 1024) {
            result.scan.error = "Report exceeds 1 MiB";
            return result;
        }
    }
    if (file_input.bad()) {
        result.scan.error = "Cannot read report";
        return result;
    }
#endif
    std::istringstream input(content);
    std::string line;
    bool header = false, ports_seen = false, failed = false;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] != '@') continue;
        std::istringstream fields(line);
        std::string kind, extra;
        fields >> kind;
        if (kind == "@inventory") {
            int version = 0, cancelled = 0, failure = 0, truncated = 0;
            if (header || !(fields >> version >> result.scan.completed >> result.scan.total >>
                            cancelled >> failure >> truncated) || fields >> extra || version != 1 ||
                cancelled < 0 || cancelled > 1 || failure < 0 || failure > 1 ||
                truncated < 0 || truncated > 1 || result.scan.completed > result.scan.total) {
                failed = true; break;
            }
            header = true;
            result.scan.cancelled = cancelled != 0;
            result.scan.truncated = truncated != 0;
            if (failure) result.scan.error = "Original scan failed";
        } else if (kind == "@ports") {
            std::string value;
            if (ports_seen || !(fields >> value) || fields >> extra) { failed = true; break; }
            auto ports = parse_port_list(value);
            if (!ports.error.empty() || ports.ports.size() > 8) { failed = true; break; }
            result.scan.ports = std::move(ports.ports);
            ports_seen = true;
        } else if (kind == "@target") {
            std::string address;
            if (!(fields >> address) || fields >> extra || !single_numeric_address(address) ||
                result.scan.targets.size() >= 256 ||
                std::find(result.scan.targets.begin(), result.scan.targets.end(), address) != result.scan.targets.end()) {
                failed = true; break;
            }
            result.scan.targets.push_back(address);
        } else if (kind == "@comparison") {
            std::string note, baseline;
            if (!result.comparison_note.empty() || !(fields >> std::quoted(note) >> std::quoted(baseline)) ||
                fields >> extra || note.empty() || note.size() > 80 || !valid_filename(baseline)) {
                failed = true; break;
            }
            result.comparison_note = note;
            result.comparison_baseline = baseline;
        } else if (kind == "@arp") {
            std::string warning;
            if (!result.arp_warning.empty() || !(fields >> std::quoted(warning)) || fields >> extra ||
                warning.empty() || warning.size() > 80) { failed = true; break; }
            result.arp_warning = warning;
        } else if (kind == "@host") {
            InventoryHost host;
            std::string ports;
            if (!(fields >> host.address >> host.cached_mac >> host.interface_name >> ports) ||
                !single_numeric_address(host.address) ||
                result.hosts.size() >= 512) { failed = true; break; }
            fields >> std::ws;
            if (!fields.eof()) {
                int responded = -1;
                if (!(fields >> responded) || fields >> extra || (responded != 0 && responded != 1)) {
                    failed = true; break;
                }
                host.responded = responded != 0;
            }
            if (host.cached_mac == "-") host.cached_mac.clear();
            else if (!usable_mac(host.cached_mac)) { failed = true; break; }
            if (host.interface_name == "-") host.interface_name.clear();
            if (ports != "-") {
                auto parsed = parse_port_list(ports);
                if (!parsed.error.empty() || parsed.ports.size() > 8) { failed = true; break; }
                host.open_ports = std::move(parsed.ports);
            }
            if (std::any_of(result.hosts.begin(), result.hosts.end(), [&](const InventoryHost& item) {
                    return item.address == host.address;
                })) { failed = true; break; }
            result.hosts.push_back(std::move(host));
        } else if (kind == "@service") {
            std::string address, service, version;
            unsigned port = 0;
            if (!(fields >> address >> port >> std::quoted(service) >> std::quoted(version)) ||
                fields >> extra || port == 0 || port > 65535 ||
                service.size() > 80 || version.size() > 80) { failed = true; break; }
            auto host = std::find_if(result.hosts.begin(), result.hosts.end(),
                [&](const InventoryHost& item) { return item.address == address; });
            if (host == result.hosts.end() || host->services.size() >= 8 ||
                std::find(result.scan.ports.begin(), result.scan.ports.end(), port) == result.scan.ports.end() ||
                std::any_of(host->services.begin(), host->services.end(),
                            [&](const NmapService& item) { return item.port == port; })) {
                failed = true; break;
            }
            host->services.push_back({static_cast<uint16_t>(port), service, version});
        } else if (kind == "@os") {
            std::string address, guess;
            if (!(fields >> address >> std::quoted(guess)) || fields >> extra || guess.size() > 80) {
                failed = true; break;
            }
            auto host = std::find_if(result.hosts.begin(), result.hosts.end(),
                [&](const InventoryHost& item) { return item.address == address; });
            if (host == result.hosts.end() || !host->os_guess.empty()) { failed = true; break; }
            host->os_guess = guess;
        } else if (kind == "@notes") {
            std::string address, label, note, due;
            if (!(fields >> address >> std::quoted(label) >> std::quoted(note)) ||
                label.size() > 80 || note.size() > 80) { failed = true; break; }
            fields >> std::ws;
            if (!fields.eof() && (!(fields >> std::quoted(due)) || fields >> extra ||
                                  (!due.empty() && !valid_inventory_date(due)))) {
                failed = true; break;
            }
            auto host = std::find_if(result.hosts.begin(), result.hosts.end(),
                [&](const InventoryHost& item) { return item.address == address; });
            if (host == result.hosts.end() || !host->label.empty() || !host->update_note.empty() ||
                !host->update_due.empty()) {
                failed = true; break;
            }
            host->label = label;
            host->update_note = note;
            host->update_due = due;
        } else if (kind == "@router") {
            std::string address, name, mac;
            if (!(fields >> address >> std::quoted(name) >> std::quoted(mac)) || fields >> extra ||
                name.size() > 80 || (!mac.empty() && !usable_mac(mac))) { failed = true; break; }
            auto host = std::find_if(result.hosts.begin(), result.hosts.end(),
                [&](const InventoryHost& item) { return item.address == address; });
            if (host == result.hosts.end() || host->router_listed) {
                failed = true; break;
            }
            host->router_name = name;
            host->router_mac = mac;
            host->router_listed = true;
        } else {
            failed = true; break;
        }
    }
    if (!header || !ports_seen || result.scan.targets.empty() ||
        std::any_of(result.hosts.begin(), result.hosts.end(), [&](const InventoryHost& host) {
            return (host.responded &&
                    std::find(result.scan.targets.begin(), result.scan.targets.end(), host.address) ==
                        result.scan.targets.end()) ||
                   (!host.responded && !host.open_ports.empty()) ||
                   std::any_of(host.open_ports.begin(), host.open_ports.end(), [&](uint16_t port) {
                       return std::find(result.scan.ports.begin(), result.scan.ports.end(), port) ==
                              result.scan.ports.end();
                   });
        })) failed = true;
    if (failed) {
        result = {};
        result.scan.error = "Invalid or unsupported discovery report";
    } else {
        result.scan.tcp = true;
    }
    return result;
}

std::string delete_network_inventory(const std::filesystem::path& directory,
                                     const std::string& name) {
    if (!valid_filename(name)) return "Invalid report name";
    const auto folder_error = ensure_private_directory(directory);
    if (!folder_error.empty()) return folder_error;
    const auto path = directory / std::filesystem::u8path(name);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status))
        return error ? "Cannot inspect report: " + error.message() : "Report is not a regular file";
    if (!std::filesystem::remove(path, error))
        return error ? "Cannot delete report: " + error.message() : "Report was not deleted";
    return {};
}

InventoryBaseline load_inventory_baseline(const std::filesystem::path& directory, bool load_report) {
    InventoryBaseline result;
    result.error = ensure_private_directory(directory);
    if (!result.error.empty()) return result;
    const auto path = directory / kBaselineFile;
    std::string content;
#if !defined(_WIN32)
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) result.error = std::string("Cannot open baseline: ") + std::strerror(errno);
        return result;
    }
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size > 128) {
        close(fd);
        result.error = "Baseline selection must be a regular file under 128 bytes";
        return result;
    }
    char buffer[129];
    for (;;) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            result.error = std::string("Cannot read baseline: ") + std::strerror(errno);
            close(fd);
            return result;
        }
        if (count == 0) break;
        content.append(buffer, static_cast<std::size_t>(count));
        if (content.size() > 128) {
            close(fd);
            result.error = "Baseline selection exceeds 128 bytes";
            return result;
        }
    }
    if (close(fd) != 0) {
        result.error = std::string("Cannot close baseline: ") + std::strerror(errno);
        return result;
    }
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
        return result;
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::file_size(path, error) > 128 || error) {
        result.error = "Baseline selection must be a regular file under 128 bytes";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "Cannot open baseline selection";
        return result;
    }
    char buffer[129];
    input.read(buffer, sizeof(buffer));
    content.assign(buffer, static_cast<std::size_t>(input.gcount()));
    if (input.bad() || content.size() > 128) {
        result.error = "Cannot read baseline selection";
        return result;
    }
#endif
    if (!valid_filename(content)) {
        result.error = "Invalid baseline selection";
        return result;
    }
    if (!load_report) {
        result.name = std::move(content);
        return result;
    }
    result.inventory = load_network_inventory(directory, content);
    if (!result.inventory.scan.error.empty() || result.inventory.scan.targets.empty()) {
        result.error = "Baseline report unavailable: " +
            (result.inventory.scan.error.empty() ? std::string("invalid report") : result.inventory.scan.error);
        result.inventory = {};
        return result;
    }
    result.name = std::move(content);
    return result;
}

std::string save_inventory_baseline(const std::filesystem::path& directory,
                                    const std::string& name) {
    if (!valid_filename(name) || name.size() > 128) return "Invalid baseline report name";
    const auto inventory = load_network_inventory(directory, name);
    if (!inventory.scan.error.empty() || inventory.scan.targets.empty())
        return inventory.scan.error.empty() ? "Invalid baseline report" : inventory.scan.error;
    auto file = open_unique_file(directory, ".inventory-baseline-", ".tmp");
    if (!file.error.empty()) return file.error;
    const bool written = std::fwrite(name.data(), 1, name.size(), file.stream.get()) == name.size();
    const bool closed = std::fclose(file.stream.release()) == 0;
    if (!written || !closed) return "Cannot write baseline selection" + remove_incomplete_file(file.path);
    const auto target = directory / kBaselineFile;
#if defined(_WIN32)
    if (!MoveFileExW(file.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return "Cannot replace baseline selection: " +
            std::system_category().message(static_cast<int>(GetLastError())) + remove_incomplete_file(file.path);
#else
    if (std::rename(file.path.c_str(), target.c_str()) != 0) {
        const std::string error = std::strerror(errno);
        return "Cannot replace baseline selection: " + error + remove_incomplete_file(file.path);
    }
#endif
    return {};
}

std::string clear_inventory_baseline(const std::filesystem::path& directory,
                                     const std::string& deleted_name) {
    const auto baseline = load_inventory_baseline(directory, false);
    if (!baseline.error.empty()) return baseline.error;
    if (baseline.name != deleted_name) return {};
    std::error_code error;
    if (!std::filesystem::remove(directory / kBaselineFile, error))
        return error ? "Cannot clear baseline: " + error.message() : "Baseline selection was not removed";
    return {};
}

InventoryComparison compare_network_inventories(const NetworkInventory& baseline,
                                                const NetworkInventory& current) {
    InventoryComparison comparison;
    if (baseline.scan.targets != current.scan.targets || baseline.scan.ports != current.scan.ports)
        return comparison;
    comparison.comparable = true;
    const auto detail = [&](std::string value) {
        if (comparison.details.size() < 512) comparison.details.push_back(std::move(value));
        else comparison.details_truncated = true;
    };
    for (const auto& host : current.hosts) {
        if (!host.responded) continue;
        const auto old = std::find_if(baseline.hosts.begin(), baseline.hosts.end(),
            [&](const InventoryHost& item) { return item.responded && item.address == host.address; });
        if (old == baseline.hosts.end()) {
            ++comparison.newly_responding;
            detail("Newly responding: " + host.address);
            comparison.newly_open_ports += host.open_ports.size();
            for (const auto port : host.open_ports)
                detail("Newly observed TCP " + std::to_string(port) + " open: " + host.address);
            continue;
        }
        if (!host.cached_mac.empty() && !old->cached_mac.empty() &&
            !equal_mac(host.cached_mac, old->cached_mac)) {
            ++comparison.mac_changed;
            detail("Cached MAC differs for " + host.address + ": " + old->cached_mac +
                   " -> " + host.cached_mac);
        }
        for (const auto port : host.open_ports)
            if (std::find(old->open_ports.begin(), old->open_ports.end(), port) == old->open_ports.end()) {
                ++comparison.newly_open_ports;
                detail("Newly observed TCP " + std::to_string(port) + " open: " + host.address);
            }
        for (const auto& service : host.services) {
            const auto prior = std::find_if(old->services.begin(), old->services.end(),
                [&](const NmapService& item) { return item.port == service.port; });
            if (prior != old->services.end() &&
                (prior->service != service.service || prior->version != service.version)) {
                ++comparison.service_changes;
                detail("Nmap service evidence differs: " + host.address + " TCP " +
                       std::to_string(service.port));
            }
        }
    }
    for (const auto& host : baseline.hosts)
        if (host.responded && std::none_of(current.hosts.begin(), current.hosts.end(),
                         [&](const InventoryHost& item) { return item.responded && item.address == host.address; }))
        {
            ++comparison.not_seen;
            detail("Not seen (unknown): " + host.address);
        }
    return comparison;
}

PortTargets exclude_inventory_targets(const PortTargets& targets, const std::string& exclusions) {
    if (!targets.error.empty() || targets.addresses.empty()) return {{}, "Invalid targets"};
    PortTargets result = targets;
    if (exclusions.empty()) return result;
    std::size_t start = 0;
    while (start < exclusions.size()) {
        const auto comma = exclusions.find(',', start);
        const auto address = exclusions.substr(start, comma == std::string::npos ? comma : comma - start);
        if (!single_numeric_address(address)) return {{}, "Exclusions must be numeric IPs"};
        const auto found = std::find(result.addresses.begin(), result.addresses.end(), address);
        if (found == result.addresses.end()) return {{}, "Excluded IP missing or repeated"};
        result.addresses.erase(found);
        if (comma == std::string::npos) break;
        start = comma + 1;
        if (start == exclusions.size()) return {{}, "Invalid exclusions"};
    }
    if (result.addresses.empty()) return {{}, "All targets excluded"};
    return result;
}

bool valid_inventory_date(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return false;
    for (std::size_t i = 0; i < date.size(); ++i)
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return false;
    const int year = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    const int day = std::stoi(date.substr(8, 2));
    if (year == 0 || month < 1 || month > 12 || day < 1) return false;
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

std::string inventory_local_date() {
    const auto now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) return {};
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &now) != 0) return {};
#else
    if (!localtime_r(&now, &local)) return {};
#endif
    char text[11]{};
    return std::strftime(text, sizeof(text), "%Y-%m-%d", &local) == 10 ? text : "";
}

RouterClients parse_router_clients(const std::string& csv) {
    RouterClients result;
    if (csv.size() > 64 * 1024) {
        result.error = "Router export exceeds 64 KiB";
        return result;
    }
    std::istringstream input(csv);
    std::string line;
    std::size_t row = 0;
    while (std::getline(input, line)) {
        ++row;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (row == 1) {
            if (line != "ip,mac,hostname") result.error = "Expected header: ip,mac,hostname";
            if (!result.error.empty()) return result;
            continue;
        }
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false, closed = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(line[i]);
            if (ch < 32 || ch >= 127) { result.error = "Router export must be ASCII"; break; }
            if (quoted) {
                if (ch == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') { field += '"'; ++i; }
                    else { quoted = false; closed = true; }
                } else field += static_cast<char>(ch);
            } else if (ch == ',') {
                fields.push_back(field); field.clear(); closed = false;
            } else if (ch == '"' && field.empty() && !closed) {
                quoted = true;
            } else if (ch == '"' || closed) {
                result.error = "Invalid quoted router CSV";
                break;
            } else field += static_cast<char>(ch);
        }
        fields.push_back(field);
        if (!result.error.empty() || quoted || fields.size() != 3 ||
            !single_numeric_address(fields[0]) ||
            (!fields[1].empty() && !usable_mac(fields[1])) ||
            fields[2].size() > 80 || result.entries.size() >= 256 ||
            std::any_of(result.entries.begin(), result.entries.end(),
                        [&](const RouterClient& item) { return item.address == fields[0]; })) {
            result.entries.clear();
            if (result.error.empty()) result.error = "Invalid or duplicate router CSV row " + std::to_string(row);
            return result;
        }
        result.entries.push_back({fields[0], fields[1], fields[2]});
    }
    if (row == 0) result.error = "Router export is empty";
    return result;
}

RouterClients load_router_clients(const std::filesystem::path& directory) {
    RouterClients result;
    result.error = ensure_private_directory(directory);
    if (!result.error.empty()) return result;
    const auto path = directory / "router-clients.csv";
    std::string content;
#if !defined(_WIN32)
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { result.error = std::string("Cannot open router export: ") + std::strerror(errno); return result; }
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size > 64 * 1024) {
        close(fd); result.error = "Router export must be a regular file under 64 KiB"; return result;
    }
    char buffer[2048];
    for (;;) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result.error = std::string("Cannot read router export: ") + std::strerror(errno); break; }
        if (!count) break;
        content.append(buffer, static_cast<std::size_t>(count));
        if (content.size() > 64 * 1024) { result.error = "Router export exceeds 64 KiB"; break; }
    }
    close(fd);
    if (!result.error.empty()) return result;
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::file_size(path, error) > 64 * 1024 || error) {
        result.error = "Router export must be a regular file under 64 KiB"; return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { result.error = "Cannot open router export"; return result; }
    char buffer[2048];
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        content.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (content.size() > 64 * 1024) { result.error = "Router export exceeds 64 KiB"; return result; }
    }
    if (input.bad()) { result.error = "Cannot read router export"; return result; }
#endif
    return parse_router_clients(content);
}

RouterImportResult add_router_evidence(NetworkInventory& inventory, const RouterClients& clients) {
    RouterImportResult result;
    if (!clients.error.empty()) {
        result.error = clients.error;
        return result;
    }
    NetworkInventory updated = inventory;
    for (auto& host : updated.hosts) {
        host.router_name.clear();
        host.router_mac.clear();
        host.router_listed = false;
    }
    updated.hosts.erase(
        std::remove_if(updated.hosts.begin(), updated.hosts.end(), [](const InventoryHost& host) {
            return !host.responded && host.label.empty() && host.update_note.empty() &&
                   host.update_due.empty() && host.services.empty() && host.os_guess.empty();
        }), updated.hosts.end());
    for (const auto& client : clients.entries) {
        auto host = std::find_if(updated.hosts.begin(), updated.hosts.end(),
            [&](const InventoryHost& item) { return item.address == client.address; });
        if (host == updated.hosts.end()) {
            if (updated.hosts.size() >= 512) {
                result.error = "Inventory limit reached (512); import not applied";
                return result;
            }
            InventoryHost imported;
            imported.address = client.address;
            imported.responded = false;
            updated.hosts.push_back(std::move(imported));
            host = updated.hosts.end() - 1;
        }
        host->router_name = client.name;
        host->router_mac = client.mac;
        host->router_listed = true;
        if (host->responded) ++result.matched;
        else ++result.imported_only;
    }
    result.retained_without_record = std::count_if(updated.hosts.begin(), updated.hosts.end(),
        [](const InventoryHost& host) { return !host.responded && !host.router_listed; });
    inventory = std::move(updated);
    return result;
}

bool inventory_mac_conflict(const InventoryHost& host) {
    return !host.cached_mac.empty() && !host.router_mac.empty() &&
           !equal_mac(host.cached_mac, host.router_mac);
}

} // namespace platform
