#include "feature_catalog.h"

namespace platform {

FeatureList network_features() {
    return {{
        {"Interfaces", "View Wi-Fi, Ethernet, and USB-network state", FeatureRisk::Safe, true},
        {"Gateway", "View the configured default gateway", FeatureRisk::Safe, true},
        {"Ping", "Check one manually entered host", FeatureRisk::Safe, true},
        {"DNS", "Resolve one manually entered hostname", FeatureRisk::Safe, true},
        {"Port scan", "Disabled", FeatureRisk::Authorized, false},
        {"Ethernet", "View and troubleshoot wired network state", FeatureRisk::Safe, true},
        {"ARP table", "Read the local address table", FeatureRisk::Safe, true},
        {"Diagnostics", "Save a sanitized local support report", FeatureRisk::Safe, true},
    }};
}

FeatureList wireless_features() {
    return {{
        {"Wi-Fi status", "Read local connection state", FeatureRisk::Safe, true},
        {"Signal status", "Read connected Wi-Fi signal state", FeatureRisk::Safe, true},
        {"Bluetooth status", "Read local adapter state", FeatureRisk::Safe, true},
        {"GATT", "Unavailable in this build; no Bluetooth GATT workflow is configured", FeatureRisk::Authorized, false},
        {"Tracker scan", "Unavailable in this build; device discovery is not enabled", FeatureRisk::Authorized, false},
        {"Spectrum", "Unavailable without supported external radio hardware", FeatureRisk::Experimental, false},
        {"Monitor mode", "Unavailable without supported kernel and hardware support", FeatureRisk::Experimental, false},
        {"Transmit", "Disabled; requires supported hardware and explicit authorization", FeatureRisk::Experimental, false},
    }};
}

FeatureList infrared_features() {
    return {{
        {"IR receive", "Unavailable; this build has no IR device driver", FeatureRisk::Authorized, false},
        {"IR replay", "Unavailable; this build has no IR device driver", FeatureRisk::Authorized, false},
        {"IR remote", "Unavailable; this build has no IR device driver", FeatureRisk::Authorized, false},
        {"IR clone", "Unavailable; this build has no IR device driver", FeatureRisk::Authorized, false},
        {"LED test", "Unavailable; this build has no IR device driver", FeatureRisk::Experimental, false},
        {"Pranks", "Disruptive transmission is disabled", FeatureRisk::Experimental, false},
        {"Remote learn", "Unavailable; this build has no IR device driver", FeatureRisk::Authorized, false},
        {"Transmit gate", "Transmission is disabled", FeatureRisk::Safe, false},
    }};
}

FeatureList expansion_features() {
    return {{
        {"Grove I2C", "Not available in this build", FeatureRisk::Safe, false},
        {"Grove UART", "Unavailable; no UART device driver is configured", FeatureRisk::Authorized, false},
        {"SPI", "Unavailable; no supported SPI hats are configured", FeatureRisk::Authorized, false},
        {"GPIO", "Unavailable; no GPIO pins are exposed by this build", FeatureRisk::Authorized, false},
        {"LoRa", "Receive or transmit only with an attached Cap", FeatureRisk::Experimental, false},
        {"CC1101", "Sub-GHz requires separate authorized Cap", FeatureRisk::Experimental, false},
        {"NFC/RFID", "Requires a supported external Cap", FeatureRisk::Experimental, false},
        {"USB host", "Not available in this build", FeatureRisk::Safe, false},
    }};
}

FeatureList tools_features() {
    return {{
        {"Files", "Count entries in the home and media folders", FeatureRisk::Safe, true},
        {"Calculator", "Local arithmetic calculator", FeatureRisk::Safe, true},
        {"Clock", "Display local system time", FeatureRisk::Safe, true},
        {"Morse", "Generate or decode Morse", FeatureRisk::Safe, true},
        {"Uptime", "Display system uptime", FeatureRisk::Safe, true},
        {"Stopwatch", "Measure a local elapsed interval", FeatureRisk::Safe, true},
        {"MAC addresses", "List local interface MAC addresses", FeatureRisk::Safe, true},
        {"Diagnostics", "Save a sanitized local support report", FeatureRisk::Safe, true},
    }};
}

FeatureList media_features() {
    return {{
        {"Camera", "Unavailable; no camera capture workflow is configured", FeatureRisk::Safe, false},
        {"Recorder", "Unavailable; no audio capture workflow is configured", FeatureRisk::Authorized, false},
        {"Music", "Unavailable; no audio playback workflow is configured", FeatureRisk::Safe, false},
        {"Piano", "Unavailable; no instrument workflow is configured", FeatureRisk::Safe, false},
        {"Album art", "Unavailable; no local music metadata workflow is configured", FeatureRisk::Safe, false},
        {"Microphone", "Unavailable; no audio capture workflow is configured", FeatureRisk::Safe, false},
        {"Speaker", "Unavailable; no audio playback workflow is configured", FeatureRisk::Safe, false},
        {"Gallery", "Unavailable; no image browsing workflow is configured", FeatureRisk::Safe, false},
    }};
}

} // namespace platform
