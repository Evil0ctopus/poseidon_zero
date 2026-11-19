#pragma once

#include <cstdint>

namespace platform {

enum class Capability {
    Wifi,
    Bluetooth,
    Ethernet,
    Battery,
    Camera,
    Imu,
    Ir,
    Rtc,
    Audio,
    Microphone,
    Grove,
    Expansion,
    Usb,
    ExternalDisplay,
};

struct CapabilityStatus {
    bool present{false};
    bool enabled{false};
    const char* detail{nullptr};
};

CapabilityStatus read_capability(Capability capability);

} // namespace platform
