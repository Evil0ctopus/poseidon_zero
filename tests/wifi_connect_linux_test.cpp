#include "wifi_scan.h"

#include <gio/gio.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr const char* kDevice = "/org/freedesktop/NetworkManager/Devices/1";
constexpr const char* kAccessPoint = "/org/freedesktop/NetworkManager/AccessPoint/2";
constexpr const char* kXml = R"xml(
<node><interface name="org.freedesktop.NetworkManager">
  <method name="ActivateConnection">
    <arg type="o" direction="in"/><arg type="o" direction="in"/>
    <arg type="o" direction="in"/><arg type="o" direction="out"/>
  </method>
  <method name="AddAndActivateConnection2">
    <arg type="a{sa{sv}}" direction="in"/><arg type="o" direction="in"/>
    <arg type="o" direction="in"/><arg type="a{sv}" direction="in"/>
    <arg type="o" direction="out"/><arg type="o" direction="out"/>
    <arg type="a{sv}" direction="out"/>
  </method>
</interface></node>
)xml";

struct FakeNetworkManager {
    std::atomic_int calls{0};
    std::atomic_bool valid{true};
};

void on_method(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
               GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
    auto& fake = *static_cast<FakeNetworkManager*>(data);
    ++fake.calls;
    if (std::string(method) == "ActivateConnection") {
        const gchar *profile, *device, *ap;
        g_variant_get(parameters, "(&o&o&o)", &profile, &device, &ap);
        fake.valid = std::string(profile) == "/" && std::string(device) == kDevice &&
                     std::string(ap) == kAccessPoint;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", "/active/1"));
        return;
    }
    if (fake.calls == 4) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.NetworkManager.Test", "test-passphrase denied");
        return;
    }
    GVariant *settings = nullptr, *options = nullptr;
    const gchar *device, *ap;
    g_variant_get(parameters, "(@a{sa{sv}}&o&o@a{sv})", &settings, &device, &ap, &options);
    GVariant* security = g_variant_lookup_value(settings, "802-11-wireless-security",
                                                 G_VARIANT_TYPE("a{sv}"));
    GVariant* connection = g_variant_lookup_value(settings, "connection", G_VARIANT_TYPE("a{sv}"));
    const gchar *key_mgmt = nullptr, *psk = nullptr;
    gboolean autoconnect = TRUE;
    const bool common = std::string(device) == kDevice && std::string(ap) == kAccessPoint &&
                        connection && g_variant_lookup(connection, "autoconnect", "b", &autoconnect) &&
                        !autoconnect;
    fake.valid = fake.calls == 3 ? common && !security
        : common && security &&
          g_variant_lookup(security, "key-mgmt", "&s", &key_mgmt) &&
          g_variant_lookup(security, "psk", "&s", &psk) &&
          std::string(key_mgmt) == (fake.calls == 5 ? "sae" : "wpa-psk") &&
          std::string(psk) == "test-passphrase";
    if (security) g_variant_unref(security);
    if (connection) g_variant_unref(connection);
    g_variant_unref(settings);
    g_variant_unref(options);
    GVariantBuilder result;
    g_variant_builder_init(&result, G_VARIANT_TYPE("a{sv}"));
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(ooa{sv})", "/settings/1", "/active/1", &result));
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
        std::cerr << "Cannot connect to test bus: " << error->message << '\n';
        g_error_free(error);
        return 1;
    }
    GVariant* owned = g_dbus_connection_call_sync(
        server, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", "org.freedesktop.NetworkManager", 0),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (!owned) {
        std::cerr << "Cannot own test service: " << error->message << '\n';
        g_error_free(error);
        return 1;
    }
    g_variant_unref(owned);
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kXml, &error);
    FakeNetworkManager fake;
    const GDBusInterfaceVTable vtable = {on_method, nullptr, nullptr, {nullptr}};
    if (!info || !g_dbus_connection_register_object(
            server, "/org/freedesktop/NetworkManager", info->interfaces[0], &vtable,
            &fake, nullptr, &error)) {
        std::cerr << "Cannot register test service: " << error->message << '\n';
        g_error_free(error);
        return 1;
    }
    GMainLoop* loop = g_main_loop_new(context, FALSE);
    std::thread server_thread([loop] { g_main_loop_run(loop); });
    g_main_context_pop_thread_default(context);

    platform::WifiScanNetwork network{"test", "", 0, true, 2412, kDevice, kAccessPoint, 0, 0x100};
    bool passed = platform::request_wifi_connection(network, platform::WifiConnectMode::Saved, "").empty() &&
                  fake.valid && fake.calls == 1;
    if (!passed) std::cerr << "Saved-profile request failed\n";
    if (!platform::request_wifi_connection(network, platform::WifiConnectMode::New,
                                            "test-passphrase").empty() ||
        !fake.valid || fake.calls != 2) {
        std::cerr << "New WPA profile request failed\n";
        passed = false;
    }
    if (platform::request_wifi_connection(network, platform::WifiConnectMode::New, "short").empty() ||
        fake.calls != 2) {
        std::cerr << "Invalid passwords must not be sent\n";
        passed = false;
    }
    network.secured = false;
    network.rsn_flags = 0;
    if (!platform::request_wifi_connection(network, platform::WifiConnectMode::New, "").empty() ||
        !fake.valid || fake.calls != 3) {
        std::cerr << "New open profile request failed\n";
        passed = false;
    }
    network.secured = true;
    network.rsn_flags = 0x100;
    const auto failure = platform::request_wifi_connection(
        network, platform::WifiConnectMode::New, "test-passphrase");
    if (failure.empty() || failure.find("test-passphrase") != std::string::npos ||
        failure.find("[redacted]") == std::string::npos) {
        std::cerr << "D-Bus errors must be reported without exposing passwords\n";
        passed = false;
    }
    network.rsn_flags = 0x400;
    if (!platform::request_wifi_connection(network, platform::WifiConnectMode::New,
                                            "test-passphrase").empty() ||
        !fake.valid || fake.calls != 5) {
        std::cerr << "New WPA3 profile request failed\n";
        passed = false;
    }
    network.rsn_flags = 0x200;
    if (platform::request_wifi_connection(network, platform::WifiConnectMode::New,
                                          "test-passphrase").empty() || fake.calls != 5) {
        std::cerr << "Unsupported authentication must not be sent\n";
        passed = false;
    }
    g_main_loop_quit(loop);
    server_thread.join();
    g_main_loop_unref(loop);
    g_dbus_node_info_unref(info);
    g_object_unref(server);
    g_main_context_unref(context);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
    return passed ? 0 : 1;
}
