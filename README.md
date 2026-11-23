<p align="center">
  <picture>
    <source
      media="(prefers-color-scheme: dark)"
      srcset="./screenshot/poseidon-zero-dark-320x170.png"
    />
    <source
      media="(prefers-color-scheme: light)"
      srcset="./screenshot/poseidon-zero-light-320x170.png"
    />
  <img
    src="./screenshot/poseidon-zero-light-320x170.png"
    alt="POSEIDON-Zero home dashboard in light and dark themes"
    width="320"
  />
  </picture>
</p>

<div align="center">
    <h1>POSEIDON-Zero</h1>
</div>

<div align="center">
  <p>A keyboard-friendly system and utility dashboard for the Linux-based CardputerZero.</p>
</div>

<p align="center" style="margin: 20px 0; display: flex; justify-content: center; flex-wrap: wrap; gap: 12px;">
  <img src="https://img.shields.io/badge/SDL2-173B73?style=flat-square&logo=sdl&logoColor=white" alt="SDL2" />
  <img src="https://img.shields.io/badge/CMake-064F8C?style=flat-square&logo=cmake&logoColor=white" alt="CMake" />
  <img src="https://img.shields.io/badge/LVGL-2A9FD6?style=flat-square&logo=lvgl&logoColor=white" alt="LVGL" />
  <img src="https://img.shields.io/badge/Raspberry%20Pi-A22846?style=flat-square&logo=raspberrypi&logoColor=white" alt="Raspberry Pi" />
</p>

<p align="center">
  <a href="https://discord.gg/ysQAWBUE9Q">
    <img
      src="https://img.shields.io/badge/Discord-Join%20Community-5865F2?style=flat-square&logo=discord&logoColor=white"
      alt="Join Discord"
    />
  </a>
</p>
POSEIDON-Zero brings a themed, keyboard-first interface and practical local utilities to the
CardputerZero. Desktop builds use an SDL simulator for UI preview; device builds use Linux and
LVGL platform drivers. Features without a supported implementation are marked `[OFF]`.

## Project Overview

The app uses a small MVVM-style structure around LVGL:

- **UI layer**: screens and widgets are implemented with LVGL objects. `BaseScreen` owns the page root, title bar, nav bar, and page content. Widgets derive from `BaseWidgets` and expose a `build()` entry point.
- **Data model**: `BaseModel` stores application state such as current page and theme mode.
- **View model**: `BaseViewModel` exposes model state as LVGL observer subjects and provides UI actions such as page switching, counter updates, theme toggle, and quit request.
- **Reactive/data binding**: `src/reactive` wraps LVGL observer subjects and common bindings so labels, styles, flags, widget values, and events can be connected to application state.
- **Data flow**: user input triggers widget callbacks, callbacks update `BaseViewModel`, view model publishes subjects, and bound UI objects refresh automatically through LVGL observers.
- **Platform layer**: platform code owns Linux input integration and other hardware-facing services. The nav bar maps hardware/keyboard keys `4` to `8`, plus `ESC` for quit.

Current UI:

- Startup: a POSEIDON splash animation; press any key to skip it. The
  `reduced_motion` setting shortens the sequence and stops the animated bars.
- Home: category launcher for system status, network status, wireless status, infrared,
  expansion, tools, and media.
- Category pages: keyboard-navigable, scrollable tool lists with status feedback for
  unavailable hardware features and visibly marked `[OFF]` parity placeholders.
  - On the Windows simulator, Network → Interfaces reports the host's adapters, not the
    CardputerZero's. Ethernet, gateway, and ARP table likewise show host data; these views do not
    initiate a scan or connection. Use `UP`/`DOWN` to browse entries and `ESC` to close the view.
    The ARP view shows at most 32 entries and marks a truncated list with `+`.
- System page: live device status, a saved theme picker, and a 12/24-hour clock setting. Display
  brightness controls are available when the Linux backlight interface is supported; the desktop
  simulator reports brightness control as unsupported.
- Theme: the navigation-bar moon/sun action quickly switches between light and dark palettes;
  **System → Themes** or **Tools → Themes** previews and saves all 11 POSEIDON-ADV palettes plus
  Zero Light.

On tool pages, use `UP`/`DOWN` or `Z`/`C` to move through the list, `ENTER` to activate an
item, and `ESC` to return home. Press `?` or the device HELP key to open keyboard help.
The WiFi menu includes the reference demo's entries in their original order. On the device,
**Scan** requests a Wi-Fi scan through NetworkManager and shows the network name, address,
signal, security, and channel when known; the desktop simulator reports it as unavailable.
NetworkManager must authorize the launcher user to request scans. A scan from the installed app
successfully listed networks on the connected device, although a non-interactive test as the same
user was denied by polkit. The app reports authorization errors and does not change system policy.
Select a result and press `ENTER`, then `S` to use a saved profile or `N` for a new one.
New open, WPA/WPA2 personal, and WPA3-SAE profiles are supported; enterprise and WEP require
an existing profile. Enter passwords on the device; they are masked and sent to NetworkManager,
which stores new profiles and their credentials. New profiles do not automatically reconnect.
Confirm with `ENTER` before switching networks. The app reports when a request is accepted, not
that a connection has completed; check Wi-Fi status afterward. Saved-profile and new
password-protected connections have been verified through the app on the connected device;
the new-open-network path has only been tested against a mock. `R` rescans.
The remaining reference-only entries are marked `[OFF]` and do not perform radio
actions. Scroll past them for the Zero's existing Wi-Fi and Bluetooth
status and GATT tools; the Zero **Wi-Fi discovery** entry opens the same scan. Letter keys jump to
reference entries; use Shift with C, X, or Z because
the unshifted keys remain navigation controls. Press `TAB` to switch between the reference entries
and the Zero tools. The position counter is relative to the selected section, and a short
description appears beneath the WiFi list.
**Infrared → IR receive** listens for one raw signal for up to 15 seconds and shows pulse and
space durations. It does not transmit or save the signal. A compatible LIRC receiver and permission
to read it are required; the desktop simulator reports reception as unsupported. The store's
`additional_hardware` permission mapping must be confirmed before publishing this feature.
**Infrared → Remote learn** receives raw timings and lets you press `S` to save a complete capture
in `~/poseidon-ir`. Files identify the carrier as unknown; they cannot be replayed by this app.
Truncated captures, including signals still arriving at the capture deadline, are not saved.
**Expansion → Grove / I2C** lists I2C bus entries exposed by the device kernel. It does not
probe addresses, open bus devices, identify Grove connectors, or control peripherals; the
desktop simulator reports this view as unsupported.
**Media → Camera** asks before capturing a single snapshot with the device's
`/usr/bin/rpicam-still`. The image stays in memory unless you press `S` to save a new JPEG in
`~/poseidon-photos`; `ESC` cancels capture or closes the image. Saving must finish before closing.
The original camera JPEG may contain camera metadata. This is not a live preview. A compatible,
accessible camera and
the camera utility are required; the desktop simulator reports it as unsupported. Physical
capture and JPEG saving have been tested through the backend on the connected CardputerZero; the
on-device viewer and save controls still need verification. Publishing requires the store camera
permission.
**Media → Music** browses local files and plays a selected regular WAV, MP3, or FLAC file at 50% volume;
press `ESC` to stop and return to the browser. Audio-device and decoding errors are shown.
**Media → Piano** plays short generated tones with `A S D F G H J K` (C4 through C5);
press `ESC` to exit. Playback errors are shown; this is a simple tone keyboard, not a sampled piano.
**Media → Album art** lets you select a local WAV, MP3, or FLAC track and view a `cover`,
`folder`, or `album` image (`.jpg`, `.jpeg`, or `.png`) alongside it, in that priority order.
Filenames are lowercase; embedded track artwork is not supported. The Gallery image limits apply.
**Media → Microphone** asks for confirmation before listening for five seconds and shows the peak
level. It does not record or save audio; `ESC` cancels. An accessible microphone is required, and
the app-store microphone permission must be configured before this feature is published. A
five-second backend test received samples on the connected device; the UI remains unverified.
**Media → Recorder** asks before recording five seconds in memory. Press `ENTER` afterward to save
a mono 16 kHz WAV file in `~/poseidon-recordings`, or `ESC` to discard it. Saving creates a new file
without overwriting an existing one; a save in progress must finish before leaving the screen.
On Unix, new photo, recording, and IR folders are created with owner-only permissions; existing
folders writable by other users are rejected rather than silently changing their permissions.
In **System → Display brightness**, `Z`/`LEFT` and `C`/`RIGHT`
adjust brightness in 5% steps; `ENTER` or `ESC` finishes. The control is available only when
exactly one readable and writable device is exposed under `/sys/class/backlight`.
**Tools → Layout toggle** cycles category lists through compact, spacious, and two-column grid
layouts; the choice is saved.

**Media → Gallery** browses local directories and opens PNG and JPEG images with `ENTER`; `ESC` returns
to the listing. Images are read on a background worker and limited to 8 MiB on disk and
1024 × 1024 pixels. Unsupported or unreadable files show an error. JPEG EXIF orientation
is not applied.

**Wireless → GATT** scans for nearby Bluetooth Low Energy advertisements for eight seconds.
Use `UP`/`DOWN` (or `Z`/`C`) to choose a device and `ENTER` to connect and list its services
and characteristics; `ESC` cancels or goes back. Only select devices you are authorized to
inspect. The browser does not pair, read or write characteristic values. It disconnects after
collecting the service list if it initiated the connection. Bluetooth and BlueZ must be enabled;
the desktop simulator cannot scan. A device must be advertising to appear in the scan. The list
shows at most 40 devices and 128 services or characteristics.

## Repository Layout

```text
.
├── assets/                 # Runtime assets used by the app
│   ├── applications/       # Desktop/app launcher metadata
│   ├── audio/              # Audio resources
│   ├── fonts/              # TTF fonts loaded through FreeType
│   └── images/             # Image resources
├── screenshot/             # Simulator screenshots for supported desktop platforms
├── src/
│   ├── app/                # Application lifecycle, asset loading, screen management
│   ├── config/             # LVGL config headers for desktop and device builds
│   ├── logger/             # Project logging wrapper
│   ├── model/              # Base application data model
│   ├── platform/           # Platform input/hardware integration
│   ├── reactive/           # LVGL observer subjects and binding helpers
│   ├── view/               # Theme, UI constants, screens, and widgets
│   │   ├── screens/        # Page-level LVGL screens
│   │   └── widgets/        # Reusable LVGL widgets such as nav bar and icon button
│   ├── viewmodel/          # BaseViewModel and UI-facing actions/state subjects
│   └── main.cpp            # Program entry point
├── CMakeLists.txt          # Build graph, dependencies, options, targets
├── CMakePresets.json       # Desktop/device configure and build presets
└── README.md
```

### Modules

- `src/app/asset_manager.*`: resolves runtime assets from the optional CMake asset root, source tree `assets/`, and installed `/usr/share/<APP_NAME>/` path.
- `src/app/screen_namager.cpp`: switches LVGL screens when the current page subject changes.
- `src/app/desktop_simulator_frame.*`: owns the desktop SDL window, renderer, high-resolution shell textures, input, and LVGL texture composition.
- `src/config/lv_conf_desktop.h`: desktop LVGL rendering settings.
- `src/config/lv_conf_cm0.h`: embedded Linux/CardputerZero LVGL settings.
- `src/platform/linux_input.*`: Linux evdev keypad support and desktop SDL keyboard routing for nav buttons.
- `src/reactive/subjects.*`: typed wrappers around LVGL subjects.
- `src/reactive/bindings.*`: helper bindings for labels, theme, widget values, flags, states, and events.
- `src/view/widgets/navbar.*`: bottom navigation bar and icon-button action mapping.
- `src/view/widgets/icon_button.*`: transparent icon button with font/color/theme support.

## Build Options

Common CMake cache options:

| Option | Default | Description |
| --- | --- | --- |
| `USE_DESKTOP` | `ON` | Build SDL desktop simulator when `ON`; build embedded Linux target when `OFF`. |
| `APP_NAME` | `poseidon_zero` | Application name used by installed asset lookup. |
| `APP_ASSETS_ROOT` | empty | Optional runtime asset root. Expected layout includes `fonts/`, `images/`, etc. |
| `APP_CONFIG_FILE` | platform default | Optional config path. Defaults to `config/poseidon-zero.conf` for desktop builds and `/etc/poseidon-zero.conf` for device builds. |
| `APP_KEY_INPUT_DEVICE` | empty | Optional Linux evdev device path, e.g. `/dev/input/event0`. Empty means auto-scan `/dev/input/event*`. |
| `APP_FRAMEBUFFER_DEVICE` | `/dev/fb0` | Linux framebuffer device used by embedded builds when `APP_USE_DRM=OFF`. |
| `APP_USE_DRM` | `OFF` | Use LVGL's Linux DRM/KMS backend instead of fbdev for embedded builds. |
| `APP_DRM_DEVICE` | `/dev/dri/card0` | DRM device path used when `APP_USE_DRM=ON`. |
| `APP_DRM_CONNECTOR_ID` | `-1` | DRM connector id used when `APP_USE_DRM=ON`; `-1` auto-selects. |

Asset lookup order:

1. `APP_ASSETS_ROOT` when provided by CMake
2. source-tree `assets/` for development
3. `/usr/share/<APP_NAME>/` for installed deployments

Theme startup behavior is configured in `config/poseidon-zero.conf`:

```ini
[application]
theme_id=0
clock_24h=yes
```

`theme_id` ranges from `0` to `11`: POSEIDON, MATRIX, E-INK, SYNTHWAVE, PHANTOM, BLOOD,
AMBER CRT, NIGHT CITY, SOLARIS, NORDIC FROST, GHOST PURPLE, and ZERO LIGHT. Existing configs
without `theme_id` continue to use `dark_mode` (`yes`/`no`, `true`/`false`, or `1`/`0`) as a
compatibility fallback. `clock_24h` accepts the same boolean values and defaults to `yes` for
existing configurations. Toggle it from **System → Clock format**; the title bar and clock tools
update on the next clock refresh, and the choice is saved with the user configuration. The
**Tools → Themes** picker previews palettes without saving until `ENTER`; `ESC` restores the
previous palette.
The startup and launcher logo is adapted from the POSEIDON-ADV demo; its license is in
`assets/images/POSEIDON-LICENSE.txt`.
Installed device builds use `/etc/poseidon-zero.conf` as the system default. Changing the theme
from the navigation bar (keyboard shortcut `6`) or saving in the theme picker writes a per-user override to
`$XDG_CONFIG_HOME/poseidon-zero/poseidon-zero.conf`, or `~/.config/poseidon-zero/poseidon-zero.conf`
when `XDG_CONFIG_HOME` is not set. The per-user file takes precedence on the next launch.

## Desktop Builds

Desktop builds are intended for fast UI development. LVGL renders at the device-native `320x170` resolution, while SDL composites that texture into a hardware-accelerated, high-DPI simulator window with the full-resolution device shell.

Current dependencies info:

| Dependency | Version | Source | Notes |
| --- | --- | --- | --- |
| LVGL | `v9.5.0` | `CMakeLists.txt` FetchContent | Main GUI framework. |
| miniaudio | `0.11.21` | `CMakeLists.txt` FetchContent | UI sound playback. |
| fmt | `12.1.0` | System package manager or `vcpkg.json` override | Logging and formatted strings. |
| libpng | `1.6.48` | System package manager or `vcpkg.json` override | PNG image decoding support for LVGL. |
| libjpeg-turbo | `3.1.3` | System package manager or `vcpkg.json` override | JPEG image decoding support for LVGL. |
| zlib | `1.3.1` | System package manager or `vcpkg.json` override | Compression dependency used by image libraries. |
| SDL2 | `2.32.54` | System package manager or `vcpkg.json` override | Desktop simulator display and input backend. |
| FreeType | `2.13.3` | System package manager or `vcpkg.json` override | Runtime TTF font rendering. |

> Windows vcpkg installs use the versions above through manifest mode, with the registry baseline pinned in `vcpkg-configuration.json`.

### Linux Desktop

Debian/Ubuntu dependencies:

```shell
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  libpng-dev \
  libjpeg-dev \
  libfmt-dev \
  libsdl2-dev \
  libfreetype-dev \
  zlib1g-dev
```

> [!NOTE]
> Minimal CMake version is 3.31.0 (in coordinate with Debian 13 trixie), for Ubuntu user you can install latest cmake via `snap`
> and run following cmake command with `/snap/bin/cmake`.


Configure:

```shell
cmake --preset linux-x86-64
```

Build:

```shell
cmake --build --preset linux-x86-64-dbg
# alternatively, you can run release build
# cmake --build --preset linux-x86-64-rel
```

Run:

```shell
./build/linux-x86-64/Debug/poseidon_zero
# or launch release build
# ./build/linux-x86-64/Release/poseidon_zero
```

### macOS Desktop

Install the Apple command-line tools:

```shell
xcode-select --install
```

Install dependencies with Homebrew:

```shell
brew install \
  cmake \
  ninja \
  libpng \
  jpeg \
  fmt \
  sdl2 \
  freetype \
  dpkg \
  zlib
```

Configure for Apple Silicon:

```shell
cmake --preset darwin-arm64
```

Build:

```shell
cmake --build --preset darwin-arm64-dbg
# alternatively, you can run release build
# cmake --build --preset darwin-arm64-rel
```

Run:

```shell
./build/darwin-arm64/Debug/poseidon_zero
# or launch release build
# ./build/darwin-arm64/Release/poseidon_zero
```

For Intel macOS, use the `darwin-x86-64` configure preset and matching build preset:

```shell
cmake --preset darwin-x86-64
cmake --build --preset darwin-x86-64-dbg
# alternatively, you can run release build
# cmake --build --preset darwin-x86-64-rel
./build/darwin-x86-64/Debug/poseidon_zero
# or launch release build
# ./build/darwin-x86-64/Release/poseidon_zero
```

### Windows Desktop

Install CMake and Ninja:

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

Install a C++ toolchain. Choose one of the following.

MSVC Build Tools:

```powershell
winget install Microsoft.VisualStudio.BuildTools
```

In the Visual Studio installer, enable **Desktop development with C++** and make sure MSVC and a Windows SDK are selected.

MinGW-w64:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```
Alternatively, you can download from [`winlibs`](https://winlibs.com/). 

Install vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

Configure environmental variables for current terminal:

```
$env:VCPKG_ROOT="C:\vcpkg"
$env:PATH="$env:VCPKG_ROOT;$env:PATH"
```

> [!TIP]
> Change `VCPKG_ROOT` to your vcpkg installation directory if it is located elsewhere.
>
> These environment variables are **session-scoped**, meaning they only apply to the current terminal session.
> Once the terminal is closed, the variables will be lost and need to be set again in a new session.
> If you want to a persist environment variable
> ```powershell
> setx VCPKG_ROOT "C:\vcpkg"
> ```


Configure and build with MSVC:

> [!IMPORTANT]
> These steps require launching CMake from a **Developer Command Prompt** or **Developer PowerShell** provided by Visual Studio.
>
> This ensures that the MSVC environment (compiler, linker, and Windows SDK paths) is properly initialized.
>
> Without this environment setup, CMake may fail to detect or correctly configure the MSVC toolchain.

```powershell
cmake --preset win32-msvc
cmake --build --preset win32-msvc-dbg
.\build\msvc\Debug\poseidon_zero.exe

# alternatively for release build
# cmake --build --preset win32-msvc-rel
# .\build\msvc\Release\poseidon_zero.exe
```

Configure and build with MinGW-w64:

> [!IMPORTANT]
> These steps require a properly configured MinGW-w64 toolchain in your system PATH.
>
> Please ensure the following compilers are available in the current terminal session:
> - gcc
> - g++
> - ld
> - ar
>
> You can verify the setup by running:
> ```powershell
> gcc --version
> g++ --version
> ```
>
> If these commands are not recognized, you must add MinGW-w64 `bin` directory to your PATH before configuring CMake.

```powershell
cmake --preset win32-mingw64
cmake --build --preset win32-mingw64-dbg
.\build\mingw64\Debug\poseidon_zero.exe

# alternatively for release build
# cmake --build --preset win32-mingw64-rel
#.\build\mingw64\Release\poseidon_zero.exe
```
> [!NOTE]
> `VCPKG` will handle the dependencies during CMake configuration process automatically, 
> this may take several minutes depend on you network connection.

## CardputerZero Cross Build

This preset builds an aarch64 Linux target from a host machine with an `aarch64-linux-gnu-gcc/g++` toolchain.
The BSP at `.cache/sdk_bsp-src` is treated as a sysroot: headers, libraries, startup objects, and pkg-config metadata are resolved from that directory first.
If that directory does not exist on the first configure, the toolchain downloads and extracts `sdk_bsp.tar.gz` automatically before CMake runs compiler checks.

Install a cross compile toolchain:

+ Debian

```shell
sudo apt install crossbuild-essential-arm64
```
+ MacOS

```shell
brew install aarch64-unknown-linux-gnu
```

> [!NOTE]
> On Windows, it's recommended to use WSL for cross build
> or you can download [cross compile tools](https://sysprogs.com/getfile/2542/raspberry64-gcc14.2.0.exe) and configure the `PATH` yourself

Configure:

```shell
cmake --preset cp0-cross
```

Build:

```shell
cmake --build --preset cp0-cross-dbg
# or release build
# cmake --build --preset cp0-cross-rel
```

When packaging from a Windows-mounted workspace in WSL, package staging on that mount may
give every installed file world-writable permissions. Stage CPack on a Linux filesystem and
inspect the resulting package permissions with `dpkg-deb -c` before installing it.

Deploy the release package to your device after filling in your device user and IP:

```shell
REMOTE_USER=<user> REMOTE_HOST=<device-ip> ./deploy.sh
```

By default, the debian package is copied to '$HOME' folder, normally it's under '/home/\<user\>'.

On your device, install the copied package with `apt` and replace the package file name with the one you copied:

```shell
sudo apt install --no-install-recommends ./poseidon-zero_0.7.0-rev1_arm64.deb
```


## Development Guide

### Navigation Keys

The first page bottom nav bar maps hardware/keyboard keys from left to right:

| Key | Nav item |
| --- | --- |
| `4` | First icon button |
| `5` | Second icon button |
| `6` | Third icon button |
| `7` | Fourth icon button |
| `8` | Fifth icon button |
| `ESC` | First icon button / quit |

The home menu is a swipeable tab carousel. Swipe left/right or use `Left`/`Right` (also
`Z`/`C`) to browse categories; `Up`/`Down` (`F`/`X`) also move between tabs. Tap **OPEN**
or press `ENTER` to open the selected category.

Category pages use keyboard-navigable lists. The Tools page scrolls to the selected entry as you
move; use `UP`/`DOWN` or `Z`/`C`, then press `ENTER` to activate it.

The counter page uses direct keyboard shortcuts:

| Key | Action |
| --- | --- |
| `ESC` | Return to the first page |
| `Left` | Decrease the counter |
| `Right` | Increase the counter |

### Files

**Tools → Files** browses local directories read-only, starting at the home directory. Use
`UP`/`DOWN` (or `F`/`X`) to select an entry, `ENTER` to open a directory, `BACKSPACE` to go
up, and `ESC` to exit or cancel a directory read. File sizes are shown when available. Use `P`/`N` to browse pages of
64 entries; a `+` after the item number indicates another page.

### Calculator

Open **Tools**, select **Calculator**, and press `ENTER`. Type an arithmetic expression using
digits, decimal points, `+`, `-`, `*`, `/`, and parentheses. Press `ENTER` to calculate,
`BACKSPACE` to edit, or `ESC` to cancel. Expressions are limited to 64 characters.

### Morse

Open **Tools**, select **Morse**, and press `ENTER`. Type letters and spaces to encode them;
prefix Morse code with `/` to decode it (for example, `/... --- ...`). Press `ENTER` to run,
`BACKSPACE` to edit, or `ESC` to cancel.

**Tools** also includes a start/stop stopwatch (`BACKSPACE` resets it), local interface MAC
addresses, and an **Identity / connection** check for the current Zero account, hostname, Wi-Fi,
Ethernet, and gateway; it runs locally and does not issue commands to a USB-connected computer.
**Flashlight** displays a full-white screen until a key is pressed. **Dice / 8-ball** switches
between dice, coin, and 8-ball with `UP`/`DOWN` (or `M`), rolls with `ENTER`, and exits with `ESC`.
**Screen test** cycles through solid display colors with any key and exits with `ESC`.
**Ambient preview** shows slowly changing colors; use `LEFT`/`RIGHT` or `Z`/`C` to select
Ocean, Sunset, or Forest, and `ESC` to exit. If `reduced_motion` is enabled in the configuration,
the preview stays still.
**Screensaver toggle** enables or disables an idle screensaver, initially disabled. It starts
after two minutes without input when no interactive tool or message is open; press any key or
tap the display to wake. **Screensaver picker** selects theme colors, a moving clock, Ocean,
Sunset, or Forest; press `ENTER` to save or `ESC` to cancel. Both settings persist.
**System → Clock format** toggles the title-bar and tool clocks between 12-hour and 24-hour time.
**Themes** previews the full palette list with `UP`/`DOWN`; press `ENTER` to save or `ESC` to
restore the prior theme. **Layout toggle** cycles through compact list, spacious list, and a
two-column grid; the choice is saved. In grid mode, `UP`/`DOWN` move between rows and
`LEFT`/`RIGHT` move between columns.
**Sound settings** controls app UI sounds independently of system volume. Sounds default to muted;
use `M` to mute or unmute, arrow keys to adjust volume, `ENTER` to save, or `ESC` to cancel.
**Media → Speaker** requests a short click at 50% app volume even when UI sounds are muted;
listen for it and check the system volume if needed. Playback errors are shown in the app.
**Media → Gallery** browses local directories and opens PNG and JPEG images. Use `ENTER` to view,
`ESC` to return to the listing, and `LEFT`/`RIGHT` or `Z`/`C` to browse while viewing. Images are
limited to 1024 × 1024 pixels and 8 MiB each.
In the desktop simulator, **Expansion → [SIM] USB keyboard** simulates connection and US-layout
keyboard reports: press `ENTER` to connect, type text, press `ENTER` to review, then `ENTER` to
confirm. `ESC` cancels or disconnects. No USB input is sent to any computer. The physical-device
USB keyboard and NFC workflows remain unavailable until their hardware interfaces are verified.
**Network → Diagnostics** saves a sanitized support report in the home folder. **System** exposes
memory, Wi-Fi, Bluetooth, Ethernet, storage, battery, clock, and
brightness. **Network** shows local interface and gateway status, allows one manually entered ping
or DNS lookup in the background, and can display the local ARP table. Port scanning and host
discovery are not provided. **Wireless** exposes local Wi-Fi, signal, and Bluetooth status;
additional ADV parity
slots are shown as `[OFF]` placeholders. Selecting one only reports that it is unavailable; no
scan, transmission, credential capture, input injection, or device-control action is performed.

### Adding a Screen

1. Add a new page enum value in `src/model/base_model.h`.
2. Add the page transition/state logic in `BaseModel` and `BaseViewModel`.
3. Create a screen under `src/view/screens/` deriving from `BaseScreen`.
4. Extend `ScreenManager` to load the new screen when the page subject changes.
5. Update `NavBar` icons and callbacks as needed.

### Adding a Widget

1. Derive from `BaseWidgets`.
2. Implement `build()` and create LVGL objects under `parent_`.
3. Use `reactive::bind_*` helpers for text, style, state, or event bindings.
4. Keep LVGL observer lifetimes object-bound whenever possible with `lv_subject_add_observer_obj` or `reactive::observe_obj`.

### Fonts and Assets

Device builds use Noto Sans CJK Medium for the default bold text and Noto Sans CJK Regular for regular-weight text. They are loaded through FreeType from:

```text
/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc
/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
```

When either system font is unavailable, the UI falls back to LVGL's built-in Montserrat fonts. Desktop builds always use the fallback fonts. Runtime icon and other asset fonts remain under:

```text
assets/fonts/
/usr/share/<APP_NAME>/fonts/
```

You can also provide an asset root at configure time:

```shell
cmake --preset darwin-arm64 -DAPP_ASSETS_ROOT=/path/to/assets
```


## Debian Package

Debian packages are produced with CPack and written to `dist/`. The package file name follows:

```text
<AppName>_<Version>_<Revision>_arm64.deb
```

Default example:

```text
dist/poseidon-zero_0.7.0-rev1_arm64.deb
```

Package layout:

| Path | Content |
| --- | --- |
| `/usr/bin/poseidon_zero` | Application executable. |
| `/etc/poseidon-zero.conf` | System-default application settings, including the startup theme. |
| `/usr/share/poseidon_zero/` | Runtime assets: fonts, images, audio. |
| `/usr/share/APPLaunch/applications/poseidon_zero.desktop` | APPLaunch launcher entry. |
| `/usr/share/APPLaunch/share/images/*.png` | APPLaunch launcher icons/fallbacks. |
| `/usr/share/doc/poseidon_zero/` | README and third-party asset license notes. |

Build and package:

```shell
cmake --preset cp0-cross
cmake --build --preset cp0-cross-rel
cpack --preset cp0-cross-deb
```

Or run the full configure, release build, and package flow with one workflow preset:

```shell
cmake --workflow --preset cp0-cross-package
```

On WSL, if the checkout is on a Windows-mounted filesystem without Linux metadata, stage
CPack on the Linux filesystem so package files do not become world-writable:

```shell
stage="$(mktemp -d)"
cd build/cp0-cross
cpack --config CPackConfig.cmake -C Release -B "$stage"
```

The package is still written to `dist/`. Check its modes with `dpkg-deb -c` before
installing; directories and the executable should be `755`, and data files `644`.

> [!NOTE]
> Use Debian based Linux host with CPack generator 
> as other OS may not resolve the dependencies correctly
> no full dpkg, dpkg-shlibdeps, objdump and readelf support

## CardputerZero AppStore Submission

1. Build the ARM64 Debian package above and validate it with
   `.agents/skills/cardputerzero-package-release/scripts/validate_app_builder.cmake`.
   The package name, project version, and architecture must match `app-builder.json`; the current
   expected release filename is `poseidon-zero_0.7.0-rev1_arm64.deb`.
2. Sign in with GitHub at <https://dev.cardputer.cc/#/upload>. The portal accepts packages up to
   80 MB. Selecting a `.deb` parses it locally; it is not uploaded until you submit.
3. Review the package checks, then complete the listing: unique app name, summary, category,
   author name, SPDX license, an available four-character share code, declared permissions, icon,
   and 1–6 screenshots at 320×170. A public source repository is optional but can prefill the
   listing from this repository's `app-builder.json`.
4. Submit the package. The portal creates a review pull request. For a first-time submission,
   include a short demo video in that pull request. After it is merged, the store index may take a
   few minutes to update. The first uploader owns the package name; later package releases must
   use a higher version.

The portal rejects packages that run with root privileges or install files in disallowed shared
paths. It also checks package metadata and maintainer scripts. Do not upload the existing
`dist/POSEIDON-Zero_0.4.0_rev1_arm64.deb`; it predates the current 0.7.0 source and is not the
release artifact.

# Contributing

If you have better framework designs on Embedded Linux or build system practice, feel free to let us know.

# License

Project released under MIT license. More license information can be found in assets folder. 
