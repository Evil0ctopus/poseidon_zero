#include "bluetooth_gatt.h"

#include <algorithm>

#if USE_DESKTOP

namespace platform {

BluetoothScanResult scan_ble_devices(const std::atomic_bool&) {
    return {{}, "Bluetooth discovery is unavailable in the desktop simulator"};
}

GattBrowseResult browse_gatt(const BluetoothDevice&, const std::atomic_bool&) {
    return {{}, "Bluetooth GATT is unavailable in the desktop simulator"};
}

} // namespace platform

#else

#include <gio/gio.h>

#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <utility>

namespace platform {
namespace {

constexpr const char* kBluez = "org.bluez";
constexpr const char* kObjects = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kProperties = "org.freedesktop.DBus.Properties";
constexpr int kCallTimeoutMs = 5000;
constexpr std::size_t kMaxDevices = 40;
constexpr std::size_t kMaxEntries = 128;

using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
using Connection = std::unique_ptr<GDBusConnection, decltype(&g_object_unref)>;
using Context = std::unique_ptr<GMainContext, decltype(&g_main_context_unref)>;
using Cancellable = std::unique_ptr<GCancellable, decltype(&g_object_unref)>;

std::string error_message(GError* error) {
    std::string message = error ? error->message : "Unknown D-Bus error";
    if (error) {
        g_error_free(error);
    }
    return message;
}

Variant call(GDBusConnection* bus,
             const char* path,
             const char* interface,
             const char* method,
             GVariant* parameters,
             const GVariantType* reply_type,
             int timeout_ms,
             std::string& error) {
    GError* failure = nullptr;
    Variant result(g_dbus_connection_call_sync(bus, kBluez, path, interface, method,
                                                parameters, reply_type,
                                                G_DBUS_CALL_FLAGS_NONE, timeout_ms,
                                                nullptr, &failure),
                   g_variant_unref);
    if (!result) {
        error = error_message(failure);
    }
    return result;
}

Connection system_bus(const std::atomic_bool& cancelled, std::string& error) {
    Cancellable cancellable(g_cancellable_new(), g_object_unref);
    std::atomic_bool finished{false};
    std::atomic_bool timed_out{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kCallTimeoutMs);
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
        const auto message = error_message(failure);
        if (!cancelled) {
            error = timed_out ? "System D-Bus connection timed out"
                              : "System D-Bus: " + message;
        }
    }
    return bus;
}

Variant managed_objects(GDBusConnection* bus, std::string& error) {
    auto result = call(bus, "/", kObjects, "GetManagedObjects", nullptr,
                       G_VARIANT_TYPE("(a{oa{sa{sv}}})"), kCallTimeoutMs, error);
    if (!result) {
        error = "BlueZ objects: " + error;
    }
    return result;
}

std::string string_property(GVariant* properties, const char* name) {
    const gchar* value = nullptr;
    return g_variant_lookup(properties, name, "&s", &value) && value ? value : "";
}

bool bool_property(GVariant* properties, const char* name) {
    gboolean value = FALSE;
    return g_variant_lookup(properties, name, "b", &value) && value;
}

std::string display_name(const std::string& name) {
    std::string safe;
    for (unsigned char c : name) {
        if (safe.size() >= 24) {
            break;
        }
        safe.push_back(c >= 32 && c <= 126 && c != '#' ? static_cast<char>(c) : '?');
    }
    return safe;
}

bool device_properties(GVariant* objects, const std::string& path, GVariant** properties) {
    Variant interfaces(g_variant_lookup_value(objects, path.c_str(),
                                               G_VARIANT_TYPE("a{sa{sv}}")),
                       g_variant_unref);
    if (!interfaces) {
        return false;
    }
    *properties = g_variant_lookup_value(interfaces.get(), "org.bluez.Device1",
                                         G_VARIANT_TYPE("a{sv}"));
    return *properties != nullptr;
}

struct SeenDevices {
    std::string adapter;
    std::set<std::string> paths;
};

void device_signal(GDBusConnection*, const gchar*, const gchar* object_path,
                   const gchar* interface_name, const gchar*, GVariant* parameters,
                   gpointer user_data) {
    auto& seen = *static_cast<SeenDevices*>(user_data);
    const gchar* path = object_path;
    bool is_device = false;
    if (std::string(interface_name) == kObjects) {
        GVariant* raw_interfaces = nullptr;
        g_variant_get(parameters, "(&o@a{sa{sv}})", &path, &raw_interfaces);
        Variant interfaces(raw_interfaces, g_variant_unref);
        Variant device(g_variant_lookup_value(interfaces.get(), "org.bluez.Device1",
                                              G_VARIANT_TYPE("a{sv}")),
                       g_variant_unref);
        is_device = static_cast<bool>(device);
    } else {
        const gchar* changed_interface = nullptr;
        GVariant* raw_changed = nullptr;
        GVariant* raw_invalidated = nullptr;
        g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface,
                      &raw_changed, &raw_invalidated);
        Variant changed(raw_changed, g_variant_unref);
        Variant invalidated(raw_invalidated, g_variant_unref);
        Variant rssi(g_variant_lookup_value(changed.get(), "RSSI", G_VARIANT_TYPE_INT16),
                     g_variant_unref);
        is_device = std::string(changed_interface) == "org.bluez.Device1" && rssi;
    }
    if (is_device && path && std::string(path).find(seen.adapter + "/dev_") == 0 &&
        seen.paths.size() < 256) {
        seen.paths.insert(path);
    }
}

bool wait_for(std::chrono::milliseconds duration, const std::atomic_bool& cancelled,
              GMainContext* context = nullptr) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!cancelled && std::chrono::steady_clock::now() < end) {
        if (context) {
            for (int events = 0; events < 32 && !cancelled; ++events) {
                if (!g_main_context_iteration(context, false)) {
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !cancelled;
}

void append_cleanup_error(std::string& error, const std::string& cleanup) {
    if (!cleanup.empty()) {
        if (!error.empty()) {
            error += "; ";
        }
        error += cleanup;
    }
}

} // namespace

BluetoothScanResult scan_ble_devices(const std::atomic_bool& cancelled) {
    BluetoothScanResult result;
    Context context(g_main_context_new(), g_main_context_unref);
    g_main_context_push_thread_default(context.get());
    auto bus = system_bus(cancelled, result.error);
    if (!bus) {
        g_main_context_pop_thread_default(context.get());
        return result;
    }

    auto response = managed_objects(bus.get(), result.error);
    if (!response) {
        g_main_context_pop_thread_default(context.get());
        return result;
    }
    GVariant* raw_objects = nullptr;
    g_variant_get(response.get(), "(@a{oa{sa{sv}}})", &raw_objects);
    Variant objects(raw_objects, g_variant_unref);
    GVariantIter iterator;
    g_variant_iter_init(&iterator, objects.get());
    const gchar* path = nullptr;
    GVariant* raw_interfaces = nullptr;
    std::string adapter;
    bool powered = false;
    while (g_variant_iter_next(&iterator, "{&o@a{sa{sv}}}", &path, &raw_interfaces)) {
        Variant interfaces(raw_interfaces, g_variant_unref);
        Variant properties(g_variant_lookup_value(interfaces.get(), "org.bluez.Adapter1",
                                                   G_VARIANT_TYPE("a{sv}")),
                           g_variant_unref);
        if (properties && (adapter.empty() || (!powered && bool_property(properties.get(), "Powered")))) {
            adapter = path;
            powered = bool_property(properties.get(), "Powered");
        }
    }
    if (adapter.empty() || !powered) {
        result.error = adapter.empty() ? "No Bluetooth adapter found" : "Bluetooth adapter is off";
        g_main_context_pop_thread_default(context.get());
        return result;
    }
    if (cancelled) {
        g_main_context_pop_thread_default(context.get());
        return result;
    }

    SeenDevices seen{adapter, {}};
    const guint added = g_dbus_connection_signal_subscribe(
        bus.get(), kBluez, kObjects, "InterfacesAdded", "/", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, device_signal, &seen, nullptr);
    const guint changed = g_dbus_connection_signal_subscribe(
        bus.get(), kBluez, kProperties, "PropertiesChanged", nullptr, "org.bluez.Device1",
        G_DBUS_SIGNAL_FLAGS_NONE, device_signal, &seen, nullptr);

    GVariantBuilder filter;
    g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("le"));
    g_variant_builder_add(&filter, "{sv}", "DuplicateData", g_variant_new_boolean(TRUE));
    auto filtered = call(bus.get(), adapter.c_str(), "org.bluez.Adapter1", "SetDiscoveryFilter",
                         g_variant_new("(a{sv})", &filter), nullptr, kCallTimeoutMs, result.error);
    bool started = false;
    if (filtered && !cancelled) {
        auto discovery = call(bus.get(), adapter.c_str(), "org.bluez.Adapter1", "StartDiscovery",
                              nullptr, nullptr, kCallTimeoutMs, result.error);
        started = static_cast<bool>(discovery);
    }
    if (started) {
        wait_for(std::chrono::seconds(8), cancelled, context.get());
        std::string cleanup;
        if (!call(bus.get(), adapter.c_str(), "org.bluez.Adapter1", "StopDiscovery",
                  nullptr, nullptr, kCallTimeoutMs, cleanup)) {
            append_cleanup_error(result.error, "Stop discovery: " + cleanup);
        }
    } else if (!result.error.empty()) {
        result.error = "BLE discovery: " + result.error;
    }
    g_dbus_connection_signal_unsubscribe(bus.get(), added);
    g_dbus_connection_signal_unsubscribe(bus.get(), changed);

    if (started && !cancelled && result.error.empty()) {
        response = managed_objects(bus.get(), result.error);
        if (response) {
            raw_objects = nullptr;
            g_variant_get(response.get(), "(@a{oa{sa{sv}}})", &raw_objects);
            objects.reset(raw_objects);
            for (const auto& device_path : seen.paths) {
                GVariant* raw_properties = nullptr;
                if (!device_properties(objects.get(), device_path, &raw_properties)) {
                    continue;
                }
                Variant properties(raw_properties, g_variant_unref);
                BluetoothDevice device{device_path, string_property(properties.get(), "Address"),
                                       display_name(string_property(properties.get(), "Name"))};
                if (!device.address.empty()) {
                    result.devices.push_back(std::move(device));
                    if (result.devices.size() == kMaxDevices) {
                        break;
                    }
                }
            }
            std::sort(result.devices.begin(), result.devices.end(),
                      [](const BluetoothDevice& a, const BluetoothDevice& b) {
                          return a.name == b.name ? a.address < b.address : a.name < b.name;
                      });
        }
    }
    g_main_context_pop_thread_default(context.get());
    return result;
}

GattBrowseResult browse_gatt(const BluetoothDevice& device, const std::atomic_bool& cancelled) {
    GattBrowseResult result;
    if (device.path.empty() || device.path.find("/org/bluez/") != 0) {
        result.error = "Invalid Bluetooth device";
        return result;
    }
    auto bus = system_bus(cancelled, result.error);
    if (!bus || cancelled) {
        return result;
    }
    auto response = managed_objects(bus.get(), result.error);
    if (!response) {
        return result;
    }
    GVariant* raw_objects = nullptr;
    g_variant_get(response.get(), "(@a{oa{sa{sv}}})", &raw_objects);
    Variant objects(raw_objects, g_variant_unref);
    GVariant* raw_properties = nullptr;
    if (!device_properties(objects.get(), device.path, &raw_properties)) {
        result.error = "Device is no longer available; scan again";
        return result;
    }
    Variant properties(raw_properties, g_variant_unref);
    const bool already_connected = bool_property(properties.get(), "Connected");
    bool attempted_connection = false;
    if (!already_connected && !cancelled) {
        attempted_connection = true;
        auto connected = call(bus.get(), device.path.c_str(), "org.bluez.Device1", "Connect",
                              nullptr, nullptr, 10000, result.error);
        if (!connected) {
            result.error = "Connect: " + result.error;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool resolved = false;
    while (!cancelled && result.error.empty() && std::chrono::steady_clock::now() < deadline) {
        response = managed_objects(bus.get(), result.error);
        if (!response) {
            break;
        }
        raw_objects = nullptr;
        g_variant_get(response.get(), "(@a{oa{sa{sv}}})", &raw_objects);
        objects.reset(raw_objects);
        raw_properties = nullptr;
        if (!device_properties(objects.get(), device.path, &raw_properties)) {
            result.error = "Device disappeared while connecting";
            break;
        }
        properties.reset(raw_properties);
        if (bool_property(properties.get(), "ServicesResolved")) {
            resolved = true;
            break;
        }
        wait_for(std::chrono::milliseconds(250), cancelled);
    }
    if (!resolved && !cancelled && result.error.empty()) {
        result.error = "Timed out waiting for GATT services";
    }
    if (resolved) {
        struct ObjectEntry { std::string path; std::string uuid; bool characteristic; };
        std::vector<ObjectEntry> found;
        GVariantIter iterator;
        g_variant_iter_init(&iterator, objects.get());
        const gchar* path = nullptr;
        GVariant* raw_interfaces = nullptr;
        while (g_variant_iter_next(&iterator, "{&o@a{sa{sv}}}", &path, &raw_interfaces)) {
            Variant interfaces(raw_interfaces, g_variant_unref);
            if (std::string(path).find(device.path + "/") != 0) {
                continue;
            }
            Variant service(g_variant_lookup_value(interfaces.get(), "org.bluez.GattService1",
                                                   G_VARIANT_TYPE("a{sv}")), g_variant_unref);
            Variant characteristic(g_variant_lookup_value(interfaces.get(), "org.bluez.GattCharacteristic1",
                                                          G_VARIANT_TYPE("a{sv}")), g_variant_unref);
            if (service || characteristic) {
                const std::string uuid = string_property(service ? service.get() : characteristic.get(), "UUID");
                if (!uuid.empty()) {
                    found.push_back({path, uuid, static_cast<bool>(characteristic)});
                }
            }
        }
        std::sort(found.begin(), found.end(), [](const ObjectEntry& a, const ObjectEntry& b) {
            return a.path < b.path;
        });
        std::string service_uuid;
        for (const auto& item : found) {
            if (!item.characteristic) {
                service_uuid = item.uuid;
            }
            result.entries.push_back({item.uuid, service_uuid, item.characteristic});
            if (result.entries.size() == kMaxEntries) {
                break;
            }
        }
    }
    if (attempted_connection) {
        std::string cleanup;
        if (!call(bus.get(), device.path.c_str(), "org.bluez.Device1", "Disconnect",
                  nullptr, nullptr, kCallTimeoutMs, cleanup)) {
            append_cleanup_error(result.error, "Disconnect: " + cleanup);
        }
    }
    return result;
}

} // namespace platform
#endif
