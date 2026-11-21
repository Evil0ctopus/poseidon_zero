#include "bluetooth_gatt.h"

#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr const char* kAdapter = "/org/bluez/hci0";
constexpr const char* kDevice = "/org/bluez/hci0/dev_00_11_22_33_44_55";
constexpr const char* kService = "/org/bluez/hci0/dev_00_11_22_33_44_55/service0001";
constexpr const char* kCharacteristic = "/org/bluez/hci0/dev_00_11_22_33_44_55/service0001/char0002";
constexpr const char* kServiceUuid = "0000180a-0000-1000-8000-00805f9b34fb";
constexpr const char* kCharacteristicUuid = "00002a29-0000-1000-8000-00805f9b34fb";

constexpr const char* kIntrospection = R"xml(
<node>
  <interface name="org.freedesktop.DBus.ObjectManager">
    <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
    <signal name="InterfacesAdded"><arg type="o"/><arg type="a{sa{sv}}"/></signal>
  </interface>
  <interface name="org.bluez.Adapter1">
    <method name="SetDiscoveryFilter"><arg type="a{sv}" direction="in"/></method>
    <method name="StartDiscovery"/>
    <method name="StopDiscovery"/>
  </interface>
  <interface name="org.bluez.Device1">
    <method name="Connect"/>
    <method name="Disconnect"/>
  </interface>
</node>
)xml";

struct FakeBluez {
    std::atomic_bool connected{false};
    std::atomic_bool reject_connection{false};
    std::atomic_int start_count{0};
    std::atomic_int stop_count{0};
    std::atomic_int connect_count{0};
    std::atomic_int disconnect_count{0};
};

void add_object(GVariantBuilder& objects, const char* path, const char* interface,
                GVariantBuilder& properties) {
    GVariantBuilder interfaces;
    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(&interfaces, "{s@a{sv}}", interface, g_variant_builder_end(&properties));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}", path, g_variant_builder_end(&interfaces));
}

GVariant* objects_reply(const FakeBluez& fake) {
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "Powered", g_variant_new_boolean(TRUE));
    add_object(objects, kAdapter, "org.bluez.Adapter1", properties);
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "Address", g_variant_new_string("00:11:22:33:44:55"));
    g_variant_builder_add(&properties, "{sv}", "Name", g_variant_new_string("Rzuber4"));
    g_variant_builder_add(&properties, "{sv}", "Connected", g_variant_new_boolean(fake.connected));
    g_variant_builder_add(&properties, "{sv}", "ServicesResolved", g_variant_new_boolean(fake.connected));
    add_object(objects, kDevice, "org.bluez.Device1", properties);
    if (fake.connected) {
        g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(kServiceUuid));
        add_object(objects, kService, "org.bluez.GattService1", properties);
        g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(kCharacteristicUuid));
        add_object(objects, kCharacteristic, "org.bluez.GattCharacteristic1", properties);
    }
    return g_variant_new("(a{oa{sa{sv}}})", &objects);
}

void method_call(GDBusConnection* connection, const gchar*, const gchar*,
                 const gchar* interface, const gchar* method, GVariant*,
                 GDBusMethodInvocation* invocation, gpointer user_data) {
    auto& fake = *static_cast<FakeBluez*>(user_data);
    const std::string name(method);
    if (std::string(interface) == "org.freedesktop.DBus.ObjectManager") {
        g_dbus_method_invocation_return_value(invocation, objects_reply(fake));
    } else if (name == "StartDiscovery") {
        ++fake.start_count;
        g_dbus_method_invocation_return_value(invocation, nullptr);
        GVariantBuilder interfaces;
        GVariantBuilder properties;
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&properties, "{sv}", "Address", g_variant_new_string("00:11:22:33:44:55"));
        g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.bluez.Device1",
                              g_variant_builder_end(&properties));
        g_dbus_connection_emit_signal(connection, nullptr, "/",
                                      "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                                      g_variant_new("(oa{sa{sv}})", kDevice, &interfaces), nullptr);
    } else if (name == "StopDiscovery") {
        ++fake.stop_count;
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (name == "Connect") {
        ++fake.connect_count;
        if (fake.reject_connection) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.bluez.Error.Failed", "Test connection rejected");
        } else {
            fake.connected = true;
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
    } else if (name == "Disconnect") {
        ++fake.disconnect_count;
        fake.connected = false;
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

} // namespace

int main() {
    GTestDBus* test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);
    const char* address = g_test_dbus_get_bus_address(test_bus);
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS", address, TRUE);
    GError* error = nullptr;
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GDBusConnection* server = g_dbus_connection_new_for_address_sync(
        address, static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    if (!server) {
        std::cerr << "Could not create test bus connection: " << error->message << '\n';
        g_error_free(error);
        return 1;
    }
    GVariant* owned = g_dbus_connection_call_sync(
        server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", "org.bluez", 0), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (!owned) {
        std::cerr << "Could not own BlueZ test name: " << error->message << '\n';
        g_error_free(error);
        return 1;
    }
    g_variant_unref(owned);
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kIntrospection, &error);
    if (!info) {
        std::cerr << "Invalid test interface: " << error->message << '\n';
        g_error_free(error);
        return 1;
    }
    FakeBluez fake;
    const GDBusInterfaceVTable vtable = {method_call, nullptr, nullptr, {nullptr}};
    const char* paths[] = {"/", kAdapter, kDevice};
    const char* names[] = {"org.freedesktop.DBus.ObjectManager", "org.bluez.Adapter1",
                           "org.bluez.Device1"};
    for (int i = 0; i < 3; ++i) {
        auto* interface = g_dbus_node_info_lookup_interface(info, names[i]);
        if (!g_dbus_connection_register_object(server, paths[i], interface, &vtable,
                                               &fake, nullptr, &error)) {
            std::cerr << "Could not register test object: " << error->message << '\n';
            g_error_free(error);
            return 1;
        }
    }
    GMainLoop* loop = g_main_loop_new(context, FALSE);
    std::thread server_thread([loop] { g_main_loop_run(loop); });
    g_main_context_pop_thread_default(context);

    std::atomic_bool cancelled{false};
    const auto scan = platform::scan_ble_devices(cancelled);
    bool ok = scan.error.empty() && scan.devices.size() == 1 &&
              scan.devices[0].name == "Rzuber4" &&
              scan.devices[0].address == "00:11:22:33:44:55";
    if (!ok) {
        std::cerr << "Discovery failed: " << scan.error << '\n';
    } else {
        const auto browse = platform::browse_gatt(scan.devices[0], cancelled);
        ok = browse.error.empty() && browse.entries.size() == 2 &&
             browse.entries[0].uuid == kServiceUuid && !browse.entries[0].characteristic &&
             browse.entries[1].uuid == kCharacteristicUuid && browse.entries[1].characteristic &&
             browse.entries[1].service_uuid == kServiceUuid;
        if (!ok) {
            std::cerr << "GATT list failed: " << browse.error << '\n';
        }
        fake.connected = true;
        const auto existing = platform::browse_gatt(scan.devices[0], cancelled);
        if (!existing.error.empty() || existing.entries.size() != 2 || !fake.connected ||
            fake.disconnect_count != 1) {
            std::cerr << "Previously connected device was not preserved\n";
            ok = false;
        }
        fake.connected = false;
        fake.reject_connection = true;
        const auto failed = platform::browse_gatt(scan.devices[0], cancelled);
        if (failed.error.empty() || !failed.entries.empty() || fake.connected) {
            std::cerr << "Connection failure was not reported\n";
            ok = false;
        }
    }
    std::thread cancel([&cancelled] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancelled = true;
    });
    const auto stopped = platform::scan_ble_devices(cancelled);
    cancel.join();
    if (!stopped.error.empty()) {
        std::cerr << "Cancelled discovery failed: " << stopped.error << '\n';
        ok = false;
    }
    g_main_loop_quit(loop);
    server_thread.join();
    if (fake.start_count != 2 || fake.stop_count != 2 ||
        fake.connect_count != 2 || fake.disconnect_count != 2 || fake.connected) {
        std::cerr << "Discovery or connection cleanup failed\n";
        ok = false;
    }
    g_main_loop_unref(loop);
    g_dbus_node_info_unref(info);
    g_object_unref(server);
    g_main_context_unref(context);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
    return ok ? 0 : 1;
}
