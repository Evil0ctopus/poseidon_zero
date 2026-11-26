#include "feature_catalog.h"

namespace platform {

NetworkFeatureList network_features() {
    return {{
        {"Interfaces", "View Wi-Fi, Ethernet, and USB-network state", FeatureRisk::Safe, true},
        {"Gateway", "View the configured default gateway", FeatureRisk::Safe, true},
        {"Ping", "Check one manually entered host", FeatureRisk::Safe, true},
        {"DNS", "Resolve one manually entered hostname", FeatureRisk::Safe, true},
        {"Port scan", "Scan authorized targets using TCP and UDP", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Ethernet", "View and troubleshoot wired network state", FeatureRisk::Safe, true},
        {"ARP table", "Read the local address table", FeatureRisk::Safe, true},
        {"Diagnostics", "Save a sanitized local support report", FeatureRisk::Safe, true},
        {"Internet check", "Check Cloudflare HTTPS connectivity", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Host discovery", "Find responding hosts on an authorized network", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Lab listener", "Log connection attempts on a selected lab address", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"HTTPS trust", "Check one authorized host's TLS certificate", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
    }};
}

FeatureList wireless_features() {
    return {{
        {"Wi-Fi status", "Read local connection state", FeatureRisk::Safe, true},
        {"Signal status", "Read connected Wi-Fi signal state", FeatureRisk::Safe, true},
        {"Bluetooth status", "Read local adapter state", FeatureRisk::Safe, true},
        {"GATT",
         "Find nearby BLE devices and browse their GATT services",
         FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Tracker scan",
         "Scan BLE for unverified FCB2 tracker indicators",
         FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Wi-Fi discovery",
         "Scan nearby access points through NetworkManager",
         FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Deauth / Evil Twin",
         "Disabled placeholder; no packet transmission or access-point impersonation is performed",
         FeatureRisk::Authorized,
         false},
        {"Portal / PMKID",
         "Disabled placeholder; no credential or handshake capture is performed",
         FeatureRisk::Authorized,
         false},
    }};
}

FeatureList infrared_features() {
    return {{
        {"IR receive", "Receive and inspect raw infrared pulses", FeatureRisk::Safe,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"IR replay", "Disabled placeholder; no IR signal can be replayed", FeatureRisk::Authorized, false},
        {"IR remote", "Disabled placeholder; no IR commands are transmitted", FeatureRisk::Authorized, false},
        {"IR clone",
         "Disabled placeholder; no IR signals can be learned or replayed",
         FeatureRisk::Authorized,
         false},
        {"Receiver check",
         "Check for a raw LIRC receiver without capturing or transmitting",
         FeatureRisk::Safe,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"IR pranks",
         "Disabled placeholder; no disruptive IR transmission is performed",
         FeatureRisk::Experimental,
         false},
        {"Remote learn",
         "Receive and save raw IR timings; no replay or carrier detection",
         FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"IR transmit",
         "Disabled placeholder; transmission is not implemented",
         FeatureRisk::Authorized,
         false},
    }};
}

FeatureList expansion_features() {
    return {{
        {"Grove / I2C",
         "List kernel-exposed I2C buses without probing devices",
         FeatureRisk::Safe,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"UART / TCP tools",
         "Disabled placeholder; no shell, tunnel, or remote-control workflow is available",
         FeatureRisk::Authorized,
         false},
        {"SPI / GPIO",
         "List Linux-exposed SPI and GPIO nodes only; no probing or pin control",
         FeatureRisk::Safe,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"LoRa / Mesh",
         "M5Stack Cap LoRa is compatible, but no LoRa or mesh driver is implemented",
         FeatureRisk::Experimental,
         false},
        {"Sub-GHz / CC1101",
         "No RF driver; NFC is separate; Hydra RF 924 Pro needs ADV's 18-pin header, no Zero adapter",
         FeatureRisk::Experimental,
         false},
        {"nRF24 / 2.4 GHz",
         "No RF driver; Hydra RF 924 Pro requires ADV's 18-pin header; no verified Zero adapter",
         FeatureRisk::Experimental,
         false},
        {"nRF52 / Zigbee",
         "Unavailable; no supported external radio hardware is configured",
         FeatureRisk::Experimental,
         false},
        {"NFC readiness",
         "Read the ST25R3916 identity only; no NFC field, card scan, or USB-HID action",
         FeatureRisk::Safe,
#if USE_DESKTOP
         false},
#else
         true},
#endif
    }};
}

ToolFeatureList tools_features() {
    return {{
        {"Files", "Browse local directories and file details", FeatureRisk::Safe, true},
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
        {"Ambient preview", "Preview slowly changing current-theme colors", FeatureRisk::Safe, true},
        {"Layout toggle", "Switch between compact, spacious, and two-column layouts", FeatureRisk::Safe, true},
        {"Screensaver toggle", "Enable or disable the inactivity screensaver", FeatureRisk::Safe, true},
        {"Screensaver picker", "Choose a color palette, theme colors, or moving clock", FeatureRisk::Safe, true},
        {"Sound settings", "Mute UI sounds or adjust their volume", FeatureRisk::Safe, true},
        {"Local scripts", "Review and run trusted local scripts", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Incident checklist", "Review incident response steps", FeatureRisk::Safe, true},
        {"Security logs", "Review, export, or delete saved security logs", FeatureRisk::Safe, true},
        {"ADS-B frame decoder", "Decode a manually entered DF17 frame; no receiver", FeatureRisk::Safe, true},
        {"ADS-B receiver", "Receive 1090 MHz via rtl_adsb; optional local logging", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
    }};
}

FeatureList media_features() {
    return {{
        {"Camera", "Capture an in-memory snapshot on supported devices", FeatureRisk::Authorized,
#if USE_DESKTOP
         false},
#else
         true},
#endif
        {"Recorder", "Capture and optionally save a short WAV recording", FeatureRisk::Authorized, true},
        {"Music", "Play a local audio file", FeatureRisk::Safe, true},
        {"Piano", "Play a simple keyboard of generated tones", FeatureRisk::Safe, true},
        {"Album art", "View cover images stored beside a local track", FeatureRisk::Safe, true},
        {"Microphone", "Measure microphone level without saving audio", FeatureRisk::Authorized, true},
        {"Speaker", "Play a short speaker test sound", FeatureRisk::Safe, true},
        {"Gallery", "Browse and view local PNG and JPEG images", FeatureRisk::Safe, true},
    }};
}

} // namespace platform
