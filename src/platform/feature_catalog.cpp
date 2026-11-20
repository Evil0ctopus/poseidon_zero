#include "feature_catalog.h"

namespace platform {

FeatureList network_features() {
    return {{
        {"Interfaces", "View Wi-Fi, Ethernet, and USB-network state", FeatureRisk::Safe, true},
        {"Gateway", "View the configured default gateway", FeatureRisk::Safe, true},
        {"Ping", "Check one manually entered host", FeatureRisk::Safe, true},
        {"DNS", "Resolve one manually entered hostname", FeatureRisk::Safe, true},
        {"Port scan / LAN recon",
         "Disabled placeholder for scans, responder/DHCP spoofing, and network hijack; no network action is run",
         FeatureRisk::Authorized,
         false},
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
        {"Wi-Fi discovery",
         "Disabled placeholder; no nearby-device scan or packet collection is available",
         FeatureRisk::Authorized,
         false},
        {"Deauth / Evil Twin",
         "Disabled placeholder; no packet transmission or access-point impersonation is performed",
         FeatureRisk::Authorized,
         false},
        {"Portal / PMKID",
         "Disabled placeholder; no credential or handshake capture is performed",
         FeatureRisk::Authorized,
         false},
        {"BLE scan / tracker",
         "Unavailable in this build; Bluetooth device discovery is not enabled",
         FeatureRisk::Authorized,
         false},
        {"BLE HID / GATT",
         "Disabled placeholder; no advertising, input injection, flooding, writes, capture, or relay is performed",
         FeatureRisk::Authorized,
         false},
    }};
}

FeatureList infrared_features() {
    return {{
        {"IR receive", "Disabled placeholder; no IR receiver is configured", FeatureRisk::Authorized, false},
        {"IR replay", "Disabled placeholder; no IR signal can be replayed", FeatureRisk::Authorized, false},
        {"IR remote", "Disabled placeholder; no IR commands are transmitted", FeatureRisk::Authorized, false},
        {"IR clone",
         "Disabled placeholder; no IR signals can be learned or replayed",
         FeatureRisk::Authorized,
         false},
        {"LED test", "Unavailable; this build has no IR device driver", FeatureRisk::Experimental, false},
        {"IR pranks",
         "Disabled placeholder; no disruptive IR transmission is performed",
         FeatureRisk::Experimental,
         false},
        {"Remote learn",
         "Disabled placeholder; no IR signal capture is available",
         FeatureRisk::Authorized,
         false},
        {"IR transmit",
         "Disabled placeholder; transmission is not implemented",
         FeatureRisk::Safe,
         false},
    }};
}

FeatureList expansion_features() {
    return {{
        {"Grove / I2C",
         "Unavailable; no supported Grove I2C device is configured",
         FeatureRisk::Safe,
         false},
        {"UART / TCP tools",
         "Disabled placeholder; no shell, tunnel, or remote-control workflow is available",
         FeatureRisk::Authorized,
         false},
        {"SPI / GPIO",
         "Unavailable; no supported SPI hats or GPIO pins are exposed",
         FeatureRisk::Authorized,
         false},
        {"LoRa / Mesh",
         "Unavailable; no supported radio or mesh workflow is configured",
         FeatureRisk::Experimental,
         false},
        {"Sub-GHz / CC1101",
         "Disabled placeholder; no radio capture, replay, brute force, or jamming is available",
         FeatureRisk::Experimental,
         false},
        {"nRF24 / 2.4 GHz",
         "Disabled placeholder; no radio scanning, HID injection, or jamming is available",
         FeatureRisk::Experimental,
         false},
        {"nRF52 / Zigbee",
         "Unavailable; no supported external radio hardware is configured",
         FeatureRisk::Experimental,
         false},
        {"NFC / USB-HID",
         "Disabled placeholder; no RFID workflow or USB-HID payload is implemented",
         FeatureRisk::Authorized,
         false},
    }};
}

ToolFeatureList tools_features() {
    return {{
        {"Files", "Count entries in the home and media folders", FeatureRisk::Safe, true},
        {"Calculator", "Local arithmetic calculator", FeatureRisk::Safe, true},
        {"Clock", "Display local system time", FeatureRisk::Safe, true},
        {"Morse", "Generate or decode Morse", FeatureRisk::Safe, true},
        {"Uptime", "Display system uptime", FeatureRisk::Safe, true},
        {"Stopwatch", "Measure a local elapsed interval", FeatureRisk::Safe, true},
        {"MAC addresses", "List local interface MAC addresses", FeatureRisk::Safe, true},
        {"Identity / connection", "Read the local account and connection status", FeatureRisk::Safe, true},
        {"Flashlight", "Show a temporary full-screen light", FeatureRisk::Safe, true},
        {"Dice / 8-ball", "Roll dice, flip a coin, or get a local random answer", FeatureRisk::Safe, true},
        {"Screen test", "Cycle local display colors", FeatureRisk::Safe, true},
        {"Themes", "Preview and choose a saved display palette", FeatureRisk::Safe, true},
        {"Ambient preview",
         "Disabled placeholder; ambient animation controls are not implemented",
         FeatureRisk::Safe,
         false},
        {"Layout toggle",
         "Disabled placeholder; alternate layouts are not implemented",
         FeatureRisk::Safe,
         false},
        {"Screensaver toggle",
         "Disabled placeholder; inactivity screensaver control is not implemented",
         FeatureRisk::Safe,
         false},
        {"Screensaver picker",
         "Disabled placeholder; no screensaver catalog is available",
         FeatureRisk::Safe,
         false},
        {"Sound settings",
         "Disabled placeholder; sound profiles and volume controls are not implemented",
         FeatureRisk::Safe,
         false},
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
