#include "wifi_scan.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

namespace platform {

bool wifi_seen_during_scan(const WifiScanResult& result, const WifiScanNetwork& network) {
    return result.scan_started_boottime >= 0 && network.last_seen_boottime >= 0 &&
           network.last_seen_boottime > result.scan_started_boottime;
}

WifiMonitorComparison compare_wifi_observations(const WifiScanResult& baseline,
                                                const WifiScanResult& current) {
    WifiMonitorComparison result;
    if (!baseline.error.empty() || !current.error.empty()) {
        result.error = "Cannot compare failed scans";
        return result;
    }
    if (baseline.scan_started_boottime < 0 || current.scan_started_boottime < 0) {
        result.error = "Scan freshness unavailable";
        return result;
    }
    using Index = std::map<std::string, const WifiScanNetwork*>;
    std::set<std::string> ambiguous;
    auto build_index = [&](const WifiScanResult& scan) {
        Index index;
        for (const auto& network : scan.networks) {
            std::string address = network.address;
            bool valid = address.size() == 17;
            for (std::size_t i = 0; valid && i < address.size(); ++i) {
                const auto ch = static_cast<unsigned char>(address[i]);
                valid = i % 3 == 2 ? ch == ':' : std::isxdigit(ch) != 0;
                address[i] = static_cast<char>(std::tolower(ch));
            }
            if (!valid || !wifi_seen_during_scan(scan, network)) {
                ++result.skipped;
                continue;
            }
            const auto inserted = index.emplace(address, &network);
            if (!inserted.second) {
                inserted.first->second = nullptr;
                ambiguous.insert(address);
                ++result.skipped;
            }
        }
        for (auto it = index.begin(); it != index.end();) {
            if (!it->second) it = index.erase(it);
            else ++it;
        }
        return index;
    };
    const auto previous = build_index(baseline);
    const auto latest = build_index(current);
    const auto comparable = [&](const auto& entry) {
        return ambiguous.count(entry.first) == 0;
    };
    result.baseline_count = std::count_if(previous.begin(), previous.end(), comparable);
    result.current_count = std::count_if(latest.begin(), latest.end(), comparable);
    for (const auto& entry : latest) {
        if (ambiguous.count(entry.first)) continue;
        const auto old = previous.find(entry.first);
        if (old == previous.end()) {
            ++result.new_addresses;
            result.details.push_back("Newly observed AP: " + entry.first);
            continue;
        }
        const auto& left = *old->second;
        const auto& right = *entry.second;
        if (left.name != right.name || left.secured != right.secured ||
            left.wpa_flags != right.wpa_flags || left.rsn_flags != right.rsn_flags ||
            (left.frequency_mhz && right.frequency_mhz && left.frequency_mhz != right.frequency_mhz)) {
            ++result.changed;
            result.details.push_back("AP properties differ: " + entry.first);
        }
    }
    for (const auto& entry : previous) {
        if (ambiguous.count(entry.first)) continue;
        if (latest.find(entry.first) == latest.end()) {
            ++result.not_seen;
            result.details.push_back("Not seen (unknown): " + entry.first);
        }
    }
    return result;
}

int wifi_channel(uint32_t frequency_mhz) {
    if (frequency_mhz == 2484) return 14;
    if (frequency_mhz >= 2412 && frequency_mhz <= 2472 &&
        (frequency_mhz - 2407) % 5 == 0) return (frequency_mhz - 2407) / 5;
    if (frequency_mhz >= 4910 && frequency_mhz <= 4980 &&
        (frequency_mhz - 4000) % 5 == 0) return (frequency_mhz - 4000) / 5;
    if (frequency_mhz >= 5005 && frequency_mhz <= 5895 &&
        (frequency_mhz - 5000) % 5 == 0) return (frequency_mhz - 5000) / 5;
    if (frequency_mhz == 5935) return 2;
    if (frequency_mhz >= 5955 && frequency_mhz <= 7115 &&
        (frequency_mhz - 5950) % 5 == 0) return (frequency_mhz - 5950) / 5;
    if (frequency_mhz >= 58320 && frequency_mhz <= 69120 &&
        (frequency_mhz - 56160) % 2160 == 0) return (frequency_mhz - 56160) / 2160;
    return -1;
}

std::vector<WifiChannelCount> summarize_wifi_channels(
    const std::vector<WifiScanNetwork>& networks) {
    std::map<std::pair<std::string, int>, std::size_t> counts;
    for (const auto& network : networks) {
        const auto frequency = network.frequency_mhz;
        const int channel = wifi_channel(frequency);
        if (channel < 0) continue;

        const char* band = frequency >= 2412 && frequency <= 2484 ? "2.4 GHz"
                         : frequency >= 4910 && frequency <= 4980 ? "4.9 GHz"
                         : frequency >= 5005 && frequency <= 5895 ? "5 GHz"
                         : frequency >= 5935 && frequency <= 7115 ? "6 GHz"
                         : frequency >= 58320 && frequency <= 69120 ? "60 GHz"
                                                                      : nullptr;
        if (band) ++counts[{band, channel}];
    }

    std::vector<WifiChannelCount> summary;
    summary.reserve(counts.size());
    for (const auto& entry : counts) {
        summary.push_back({entry.first.first, entry.first.second, entry.second});
    }
    std::sort(summary.begin(), summary.end(),
              [](const WifiChannelCount& left, const WifiChannelCount& right) {
                  if (left.access_points != right.access_points) {
                      return left.access_points > right.access_points;
                  }
                  if (left.band != right.band) return left.band < right.band;
                  return left.channel < right.channel;
              });
    return summary;
}

WifiSecurity wifi_security(const WifiScanNetwork& network) {
    const uint32_t flags = network.wpa_flags | network.rsn_flags;
    if (!network.secured && flags == 0) return WifiSecurity::Open;
    if (flags & 0x400U) return WifiSecurity::Sae;
    if (flags & 0x100U) return WifiSecurity::WpaPsk;
    return WifiSecurity::Unsupported;
}

std::string wifi_hardening_advice(const WifiScanNetwork& network) {
    constexpr uint32_t psk = 0x100U;
    constexpr uint32_t enterprise = 0x200U;
    constexpr uint32_t sae = 0x400U;
    if (!network.secured && !(network.wpa_flags | network.rsn_flags))
        return "Open network advertised\nUse WPA3 or WPA2 where possible";
    if ((network.rsn_flags & sae) && (network.rsn_flags & psk))
        return "SAE/PSK transition\nConsider SAE-only with PMF";
    if ((network.rsn_flags & sae) && network.wpa_flags)
        return "SAE plus legacy WPA\nDisable legacy WPA if possible";
    if (network.rsn_flags & sae)
        return "SAE advertised\nVerify WPA3 and PMF settings";
    if ((network.rsn_flags & psk) && network.wpa_flags)
        return "WPA2 plus legacy WPA\nDisable legacy WPA if possible";
    if (network.rsn_flags & psk)
        return "WPA2 personal advertised\nConsider WPA3 and strong keys";
    if ((network.rsn_flags & enterprise) && network.wpa_flags)
        return "Enterprise plus legacy WPA\nDisable legacy WPA if possible";
    if (network.rsn_flags & enterprise)
        return "Enterprise authentication\nVerify certificates and policy";
    if (network.wpa_flags && !network.rsn_flags)
        return "Legacy WPA advertised\nUpgrade to WPA2 or WPA3";
    return "Security not determined\nReview AP settings directly";
}

bool valid_wifi_password(WifiSecurity security, const std::string& password) {
    if (security == WifiSecurity::Open) return password.empty();
    if (security != WifiSecurity::WpaPsk && security != WifiSecurity::Sae) return false;
    if (password.size() >= (security == WifiSecurity::Sae ? 1U : 8U) &&
        password.size() <= 63)
        return std::all_of(password.begin(), password.end(), [](unsigned char ch) {
            return ch >= 32 && ch <= 126;
        });
    return security == WifiSecurity::WpaPsk && password.size() == 64 &&
           std::all_of(password.begin(), password.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                      (ch >= 'A' && ch <= 'F');
           });
}

} // namespace platform

#if USE_DESKTOP

namespace platform {

WifiScanResult scan_wifi(const std::atomic_bool& cancelled) {
    if (cancelled) {
        return {};
    }
    return {{}, "Wi-Fi scanning requires a supported device"};
}

std::string request_wifi_connection(const WifiScanNetwork&, WifiConnectMode,
                                    const std::string&) {
    return "Wi-Fi connection requires a supported device";
}

} // namespace platform

#else

#include <gio/gio.h>

#include <chrono>
#include <memory>
#include <thread>
#include <time.h>

namespace platform {
namespace {

constexpr const char* kService = "org.freedesktop.NetworkManager";
constexpr const char* kRoot = "/org/freedesktop/NetworkManager";
constexpr const char* kDevice = "org.freedesktop.NetworkManager.Device";
constexpr const char* kWireless = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* kAccessPoint = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char* kProperties = "org.freedesktop.DBus.Properties";
constexpr int kTimeoutMs = 3000;
constexpr std::size_t kMaxNetworks = 40;

using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
using Connection = std::unique_ptr<GDBusConnection, decltype(&g_object_unref)>;
using Cancellable = std::unique_ptr<GCancellable, decltype(&g_object_unref)>;

std::string take_error(GError* error) {
    std::string message = error ? error->message : "Unknown D-Bus error";
    if (error) {
        g_error_free(error);
    }
    return message;
}

Connection system_bus(const std::atomic_bool& cancelled, std::string& error) {
    Cancellable cancellable(g_cancellable_new(), g_object_unref);
    std::atomic_bool finished{false};
    std::atomic_bool timed_out{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kTimeoutMs);
        while (!finished && !cancelled && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!finished) {
            timed_out = !cancelled;
            g_cancellable_cancel(cancellable.get());
        }
    });
    GError* failure = nullptr;
    Connection bus(g_bus_get_sync(G_BUS_TYPE_SYSTEM, cancellable.get(), &failure), g_object_unref);
    finished = true;
    watchdog.join();
    if (!bus) {
        const auto detail = take_error(failure);
        if (!cancelled) {
            error = timed_out ? "System D-Bus connection timed out" : "System D-Bus: " + detail;
        }
    }
    return bus;
}

Variant call(GDBusConnection* bus, const char* path, const char* interface,
             const char* method, GVariant* parameters, const GVariantType* reply_type,
             std::string& error) {
    GError* failure = nullptr;
    Variant result(g_dbus_connection_call_sync(bus, kService, path, interface, method,
                                                parameters, reply_type, G_DBUS_CALL_FLAGS_NONE,
                                                kTimeoutMs, nullptr, &failure),
                   g_variant_unref);
    if (!result) {
        error = take_error(failure);
    }
    return result;
}

Variant property(GDBusConnection* bus, const char* path, const char* interface,
                 const char* name, std::string& error) {
    auto result = call(bus, path, kProperties, "Get", g_variant_new("(ss)", interface, name),
                       G_VARIANT_TYPE("(v)"), error);
    if (!result) {
        return Variant(nullptr, g_variant_unref);
    }
    GVariant* value = nullptr;
    g_variant_get(result.get(), "(v)", &value);
    return Variant(value, g_variant_unref);
}

std::string safe_name(GVariant* bytes) {
    gsize length = 0;
    const auto* data = static_cast<const guint8*>(g_variant_get_fixed_array(bytes, &length, 1));
    std::string name;
    for (gsize i = 0; i < length && i < 32; ++i) {
        const auto ch = data[i];
        name.push_back(ch >= 32 && ch <= 126 && ch != '#' ? static_cast<char>(ch) : '?');
    }
    return name.empty() ? "(hidden network)" : name;
}

} // namespace

WifiScanResult scan_wifi(const std::atomic_bool& cancelled) {
    WifiScanResult result;
    if (cancelled) {
        return result;
    }
    auto bus = system_bus(cancelled, result.error);
    if (!bus) {
        return result;
    }
    auto devices = call(bus.get(), kRoot, kService, "GetDevices", nullptr,
                        G_VARIANT_TYPE("(ao)"), result.error);
    if (!devices) {
        result.error = "Wi-Fi device list: " + result.error;
        return result;
    }
    GVariantIter* iterator = nullptr;
    g_variant_get(devices.get(), "(ao)", &iterator);
    const gchar* path = nullptr;
    std::string adapter;
    while (!cancelled && g_variant_iter_loop(iterator, "&o", &path)) {
        auto type = property(bus.get(), path, kDevice, "DeviceType", result.error);
        if (!type) {
            break;
        }
        if (g_variant_is_of_type(type.get(), G_VARIANT_TYPE_UINT32) &&
            g_variant_get_uint32(type.get()) == 2) {
            adapter = path;
            break;
        }
    }
    g_variant_iter_free(iterator);
    if (cancelled) {
        return {};
    }
    if (!result.error.empty()) {
        result.error = "Wi-Fi device type: " + result.error;
        return result;
    }
    if (adapter.empty()) {
        result.error = "No Wi-Fi adapter found";
        return result;
    }
    auto previous = property(bus.get(), adapter.c_str(), kWireless, "LastScan", result.error);
    if (!previous || !g_variant_is_of_type(previous.get(), G_VARIANT_TYPE_INT64)) {
        if (result.error.empty()) result.error = "Invalid LastScan value";
        return result;
    }
    if (cancelled) {
        return {};
    }
    const auto last_scan = g_variant_get_int64(previous.get());
    timespec boot_time{};
    if (clock_gettime(CLOCK_BOOTTIME, &boot_time) == 0) {
        result.scan_started_boottime = boot_time.tv_sec;
    }
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    auto requested = call(bus.get(), adapter.c_str(), kWireless, "RequestScan",
                          g_variant_new("(a{sv})", &options), G_VARIANT_TYPE("()"), result.error);
    if (!requested) {
        result.error = "Wi-Fi scan request: " + result.error;
        return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool completed = false;
    while (!cancelled && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto current = property(bus.get(), adapter.c_str(), kWireless, "LastScan", result.error);
        if (!current || !g_variant_is_of_type(current.get(), G_VARIANT_TYPE_INT64)) {
            if (result.error.empty()) result.error = "Invalid LastScan value";
            return result;
        }
        if (g_variant_get_int64(current.get()) > last_scan) {
            completed = true;
            break;
        }
    }
    if (cancelled) {
        return {};
    }
    if (!completed) {
        result.error = "Wi-Fi scan timed out";
        return result;
    }
    auto access_points = call(bus.get(), adapter.c_str(), kWireless, "GetAllAccessPoints",
                              nullptr, G_VARIANT_TYPE("(ao)"), result.error);
    if (!access_points) {
        result.error = "Wi-Fi results: " + result.error;
        return result;
    }
    g_variant_get(access_points.get(), "(ao)", &iterator);
    while (!cancelled && result.networks.size() < kMaxNetworks &&
           g_variant_iter_loop(iterator, "&o", &path)) {
        auto all = call(bus.get(), path, kProperties, "GetAll",
                        g_variant_new("(s)", kAccessPoint), G_VARIANT_TYPE("(a{sv})"),
                        result.error);
        if (!all) break;
        GVariant* raw_properties = nullptr;
        g_variant_get(all.get(), "(@a{sv})", &raw_properties);
        Variant properties(raw_properties, g_variant_unref);
        Variant ssid(g_variant_lookup_value(properties.get(), "Ssid", G_VARIANT_TYPE("ay")),
                     g_variant_unref);
        const gchar* address = nullptr;
        guint8 strength = 0;
        guint32 flags = 0, wpa = 0, rsn = 0, frequency = 0;
        gint32 last_seen = -1;
        if (!ssid || !g_variant_lookup(properties.get(), "HwAddress", "&s", &address) ||
            !g_variant_lookup(properties.get(), "Strength", "y", &strength) ||
            !g_variant_lookup(properties.get(), "Frequency", "u", &frequency) ||
            !g_variant_lookup(properties.get(), "Flags", "u", &flags) ||
            !g_variant_lookup(properties.get(), "WpaFlags", "u", &wpa) ||
            !g_variant_lookup(properties.get(), "RsnFlags", "u", &rsn)) {
            result.error = "Incomplete Wi-Fi access point data";
            break;
        }
        g_variant_lookup(properties.get(), "LastSeen", "i", &last_seen);
        result.networks.push_back({safe_name(ssid.get()), address, strength,
                                   (flags & 1U) != 0 || wpa != 0 || rsn != 0, frequency,
                                   adapter, path, wpa, rsn, last_seen});
    }
    g_variant_iter_free(iterator);
    if (cancelled) return {};
    if (!result.error.empty()) {
        result.networks.clear();
        result.error = "Wi-Fi access point: " + result.error;
    }
    return result;
}

std::string request_wifi_connection(const WifiScanNetwork& network, WifiConnectMode mode,
                                    const std::string& password) {
    if (network.device_path.empty() || network.access_point_path.empty() ||
        !g_variant_is_object_path(network.device_path.c_str()) ||
        !g_variant_is_object_path(network.access_point_path.c_str()))
        return "Invalid access point; scan again";
    const auto security = wifi_security(network);
    if (mode == WifiConnectMode::New &&
        (security == WifiSecurity::Unsupported || !valid_wifi_password(security, password)))
        return "Unsupported security or invalid password";

    const std::atomic_bool cancelled{false};
    std::string error;
    auto bus = system_bus(cancelled, error);
    if (!bus) return error;
    Variant result(nullptr, g_variant_unref);
    if (mode == WifiConnectMode::Saved) {
        result = call(bus.get(), kRoot, kService, "ActivateConnection",
                      g_variant_new("(ooo)", "/", network.device_path.c_str(),
                                    network.access_point_path.c_str()),
                      G_VARIANT_TYPE("(o)"), error);
    } else {
        GVariantBuilder settings;
        g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
        GVariantBuilder connection;
        g_variant_builder_init(&connection, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&connection, "{sv}", "autoconnect", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&settings, "{s@a{sv}}", "connection",
                              g_variant_builder_end(&connection));
        if (security != WifiSecurity::Open) {
            GVariantBuilder wireless_security;
            g_variant_builder_init(&wireless_security, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&wireless_security, "{sv}", "key-mgmt",
                                  g_variant_new_string(security == WifiSecurity::Sae ? "sae" : "wpa-psk"));
            g_variant_builder_add(&wireless_security, "{sv}", "psk",
                                  g_variant_new_string(password.c_str()));
            g_variant_builder_add(&settings, "{s@a{sv}}", "802-11-wireless-security",
                                  g_variant_builder_end(&wireless_security));
        }
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        result = call(bus.get(), kRoot, kService, "AddAndActivateConnection2",
                      g_variant_new("(a{sa{sv}}ooa{sv})", &settings,
                                    network.device_path.c_str(), network.access_point_path.c_str(),
                                    &options), G_VARIANT_TYPE("(ooa{sv})"), error);
    }
    if (!result) {
        if (!password.empty()) {
            std::size_t offset = 0;
            while ((offset = error.find(password, offset)) != std::string::npos) {
                error.replace(offset, password.size(), "[redacted]");
                offset += 10;
            }
        }
        if (error.rfind("GDBus.Error:", 0) == 0) {
            const auto separator = error.find(": ", 12);
            if (separator != std::string::npos) error.erase(0, separator + 2);
        }
        if (error.size() > 120) error.resize(120);
        return "Request error; check Wi-Fi status: " + error;
    }
    return {};
}

} // namespace platform

#endif
