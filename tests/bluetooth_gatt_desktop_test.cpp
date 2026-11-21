#include "bluetooth_gatt.h"

#include <atomic>
#include <iostream>

int main() {
    std::atomic_bool cancelled{false};
    const auto scan = platform::scan_ble_devices(cancelled);
    if (scan.error.empty() || !scan.devices.empty()) {
        std::cerr << "Desktop scan must report unsupported, not simulated devices\n";
        return 1;
    }

    const auto browse = platform::browse_gatt({}, cancelled);
    if (browse.error.empty() || !browse.entries.empty()) {
        std::cerr << "Desktop GATT must report unsupported, not simulated services\n";
        return 1;
    }
    return 0;
}
