#include "feature_catalog.h"

namespace platform {

FeatureList network_features() {
    return {{
        {"Interfaces", "View Wi-Fi, Ethernet, and USB-network state", FeatureRisk::Safe, true},
        {"Host discovery", "Discover hosts on the local subnet", FeatureRisk::Authorized, true},
        {"Ping", "Check reachability of an authorized host", FeatureRisk::Safe, true},
        {"DNS", "Resolve authorized hostnames", FeatureRisk::Safe, true},
        {"Port scan", "Scan a host owned or authorized for testing", FeatureRisk::Authorized, true},
        {"Ethernet", "View and troubleshoot wired network state", FeatureRisk::Safe, true},
        {"ARP", "Read the local address resolution table", FeatureRisk::Safe, true},
        {"Diagnostics", "Run bounded network checks", FeatureRisk::Safe, true},
    }};
}

FeatureList wireless_features() {
    return {{
        {"Wi-Fi scan", "Passive inventory with NetworkManager", FeatureRisk::Authorized, true},
        {"Signal status", "Read connected Wi-Fi signal state", FeatureRisk::Safe, true},
        {"BLE scan", "Discover Bluetooth Low Energy devices", FeatureRisk::Authorized, true},
        {"GATT", "Read or write only explicitly selected services", FeatureRisk::Authorized, false},
        {"Tracker scan", "Identify nearby tracker advertisement types", FeatureRisk::Authorized, false},
        {"Spectrum", "Requires supported external radio hardware", FeatureRisk::Experimental, false},
        {"Monitor mode", "Disabled unless kernel and hardware support it", FeatureRisk::Experimental, false},
        {"Transmit", "Disabled by default; requires explicit authorization", FeatureRisk::Experimental, false},
    }};
}

FeatureList infrared_features() {
    return {{
        {"IR receive", "Capture a remote signal", FeatureRisk::Authorized, false},
        {"IR replay", "Replay a previously captured local signal", FeatureRisk::Authorized, false},
        {"IR remote", "Use a saved remote profile", FeatureRisk::Authorized, false},
        {"IR clone", "Save an owned remote profile", FeatureRisk::Authorized, false},
        {"LED test", "Verify IR hardware safely", FeatureRisk::Experimental, false},
        {"Pranks", "Transmit-only disruptive actions disabled", FeatureRisk::Experimental, false},
        {"Remote learn", "Learn from an owned remote", FeatureRisk::Authorized, false},
        {"Transmit gate", "Explicit confirmation required for every send", FeatureRisk::Safe, true},
    }};
}

FeatureList expansion_features() {
    return {{
        {"Grove I2C", "Enumerate I2C peripherals", FeatureRisk::Safe, true},
        {"Grove UART", "Operate an attached UART device", FeatureRisk::Authorized, false},
        {"SPI", "Enumerate and test supported SPI hats", FeatureRisk::Authorized, false},
        {"GPIO", "Read and write explicitly exposed pins", FeatureRisk::Authorized, false},
        {"LoRa", "Receive or transmit only with an attached Cap", FeatureRisk::Experimental, false},
        {"CC1101", "Sub-GHz requires separate authorized Cap", FeatureRisk::Experimental, false},
        {"NFC/RFID", "Requires a supported external Cap", FeatureRisk::Experimental, false},
        {"USB host", "List USB devices and connection state", FeatureRisk::Safe, true},
    }};
}

FeatureList tools_features() {
    return {{
        {"File browser", "Browse, preview, copy, and view local files", FeatureRisk::Safe, true},
        {"Calculator", "Local calculator", FeatureRisk::Safe, false},
        {"Stopwatch", "Local timer", FeatureRisk::Safe, false},
        {"Morse", "Generate or decode Morse", FeatureRisk::Safe, false},
        {"MAC lookup", "Identify vendor from captured OUI", FeatureRisk::Safe, true},
        {"Clock", "Display RTC and system time", FeatureRisk::Safe, true},
        {"Memory", "Monitor available RAM and limits", FeatureRisk::Safe, true},
        {"Diagnostics", "Export sanitized support status", FeatureRisk::Safe, true},
    }};
}

FeatureList media_features() {
    return {{
        {"Camera", "Use the built-in or USB camera", FeatureRisk::Safe, false},
        {"Recorder", "Record local audio", FeatureRisk::Authorized, false},
        {"Music", "Play local audio", FeatureRisk::Safe, false},
        {"Piano", "Keyboard instrument", FeatureRisk::Safe, false},
        {"Album art", "Read local music metadata", FeatureRisk::Safe, false},
        {"Microphone", "Read microphone capability", FeatureRisk::Safe, true},
        {"Speaker", "Read or route audio output", FeatureRisk::Safe, true},
        {"Gallery", "Browse local images", FeatureRisk::Safe, false},
    }};
}

} // namespace platform
