#pragma once

#include "remote_id.h"
#include <atomic>
#include <string>
#include <vector>

namespace platform {

struct BluetoothDevice {
    std::string path;
    std::string address;
    std::string name;
    bool remote_id_indicator{false};
    RemoteIdData remote_id;
    bool tracker_indicator{false};
};

struct GattEntry {
    std::string uuid;
    std::string service_uuid;
    bool characteristic{false};
};

struct BluetoothScanResult {
    std::vector<BluetoothDevice> devices;
    std::string error;
};

struct GattBrowseResult {
    std::vector<GattEntry> entries;
    std::string error;
};

BluetoothScanResult scan_ble_devices(const std::atomic_bool& cancelled);
std::vector<BluetoothDevice> tracker_indicator_devices(std::vector<BluetoothDevice> devices);
GattBrowseResult browse_gatt(const BluetoothDevice& device,
                            const std::atomic_bool& cancelled);

} // namespace platform
