#include "expansion_inventory.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>

#if !USE_DESKTOP
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

bool contains_only_digits(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return character >= '0' && character <= '9';
           });
}

#if !USE_DESKTOP
int retry_ioctl(int descriptor, unsigned long request, void* value) {
    int result = 0;
    do {
        result = ::ioctl(descriptor, request, value);
    } while (result < 0 && errno == EINTR);
    return result;
}

std::string system_error(const std::string& action) {
    return action + ": " + std::strerror(errno);
}

std::string hex_byte(std::uint8_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text{"0x00"};
    text[2] = digits[value >> 4];
    text[3] = digits[value & 0x0F];
    return text;
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const {
        return value_;
    }

private:
    int value_;
};
#endif

} // namespace

ExpansionNodeKind classify_expansion_node(std::string_view name) {
    constexpr std::string_view spi_prefix = "spidev";
    if (name.substr(0, spi_prefix.size()) == spi_prefix) {
        const auto separator = name.find('.', spi_prefix.size());
        if (separator != std::string_view::npos &&
            contains_only_digits(name.substr(spi_prefix.size(),
                                             separator - spi_prefix.size())) &&
            contains_only_digits(name.substr(separator + 1))) {
            return ExpansionNodeKind::Spi;
        }
    }

    constexpr std::string_view gpio_prefix = "gpiochip";
    if (name.substr(0, gpio_prefix.size()) == gpio_prefix &&
        contains_only_digits(name.substr(gpio_prefix.size()))) {
        return ExpansionNodeKind::Gpio;
    }
    return ExpansionNodeKind::None;
}

ExpansionBusInventory read_expansion_bus_inventory() {
    ExpansionBusInventory result;
#if USE_DESKTOP
    result.error = "Unavailable in the desktop simulator";
#else
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator entry("/dev", error);
         !error && entry != end;
         entry.increment(error)) {
        std::error_code status_error;
        if (!entry->is_character_file(status_error)) {
            if (status_error) {
                error = status_error;
                break;
            }
            continue;
        }
        const auto name = entry->path().filename().string();
        switch (classify_expansion_node(name)) {
            case ExpansionNodeKind::Spi:
                result.spi_nodes.push_back("/dev/" + name);
                break;
            case ExpansionNodeKind::Gpio:
                result.gpio_chips.push_back("/dev/" + name);
                break;
            case ExpansionNodeKind::None:
                break;
        }
    }
    if (error) {
        result.spi_nodes.clear();
        result.gpio_chips.clear();
        result.error = "Could not list /dev: " + error.message();
        return result;
    }
    std::sort(result.spi_nodes.begin(), result.spi_nodes.end());
    std::sort(result.gpio_chips.begin(), result.gpio_chips.end());
#endif
    return result;
}

bool decode_st25r3916_identity(std::uint8_t value, St25r3916Identity& identity) {
    identity = {};
    const auto type = static_cast<std::uint8_t>((value >> 3) & 0x1F);
    const auto revision = static_cast<std::uint8_t>(value & 0x07);
    if (type != 0x05 || revision == 0) {
        return false;
    }
    identity = {value, revision};
    return true;
}

CapNfcProbeResult read_cap_nfc_identity() {
    CapNfcProbeResult result;
#if USE_DESKTOP
    result.error = "NFC hardware is unavailable in the desktop simulator";
#else
    constexpr char device[] = "/dev/spidev0.2";
    constexpr std::uint32_t speed_hz = 1'000'000;
    FileDescriptor descriptor(::open(device, O_RDWR | O_CLOEXEC));
    if (descriptor.get() < 0) {
        result.error = system_error(std::string("open ") + device);
        return result;
    }

    std::uint8_t mode = SPI_MODE_1;
    std::uint8_t actual_mode = 0;
    std::uint8_t bits_per_word = 8;
    std::uint8_t actual_bits_per_word = 0;
    std::uint32_t configured_speed = speed_hz;
    std::uint32_t actual_speed = 0;
    if (retry_ioctl(descriptor.get(), SPI_IOC_WR_MODE, &mode) < 0) {
        result.error = system_error(std::string("configure SPI mode on ") + device);
        return result;
    }
    if (retry_ioctl(descriptor.get(), SPI_IOC_RD_MODE, &actual_mode) < 0) {
        result.error = system_error(std::string("read SPI mode on ") + device);
        return result;
    }
    if (actual_mode != mode) {
        result.error = std::string("SPI mode readback mismatch on ") + device;
        return result;
    }
    if (retry_ioctl(descriptor.get(), SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0) {
        result.error = system_error(std::string("configure SPI word size on ") + device);
        return result;
    }
    if (retry_ioctl(descriptor.get(), SPI_IOC_RD_BITS_PER_WORD, &actual_bits_per_word) < 0) {
        result.error = system_error(std::string("read SPI word size on ") + device);
        return result;
    }
    if (actual_bits_per_word != bits_per_word) {
        result.error = std::string("SPI word-size readback mismatch on ") + device;
        return result;
    }
    if (retry_ioctl(descriptor.get(), SPI_IOC_WR_MAX_SPEED_HZ, &configured_speed) < 0) {
        result.error = system_error(std::string("configure SPI speed on ") + device);
        return result;
    }
    if (retry_ioctl(descriptor.get(), SPI_IOC_RD_MAX_SPEED_HZ, &actual_speed) < 0) {
        result.error = system_error(std::string("read SPI speed on ") + device);
        return result;
    }
    if (actual_speed != configured_speed) {
        result.error = std::string("SPI speed readback mismatch on ") + device;
        return result;
    }

    std::array<std::uint8_t, 2> transmit{0x7F, 0x00};
    std::array<std::uint8_t, 2> receive{};
    spi_ioc_transfer transfer{};
    transfer.tx_buf = reinterpret_cast<std::uintptr_t>(transmit.data());
    transfer.rx_buf = reinterpret_cast<std::uintptr_t>(receive.data());
    transfer.len = static_cast<std::uint32_t>(transmit.size());
    transfer.speed_hz = speed_hz;
    transfer.bits_per_word = bits_per_word;
    const int transferred = retry_ioctl(descriptor.get(), SPI_IOC_MESSAGE(1), &transfer);
    if (transferred < 0) {
        result.error = system_error(std::string("read ST25R3916 identity over ") + device);
        return result;
    }
    if (transferred != static_cast<int>(transmit.size())) {
        result.error = std::string("short ST25R3916 identity read over ") + device;
        return result;
    }
    if (!decode_st25r3916_identity(receive[1], result.identity)) {
        result.error = "No ST25R3916 identity on " + std::string(device) +
                       " (received " + hex_byte(receive[1]) + ")";
        return result;
    }
    result.detected = true;
#endif
    return result;
}

} // namespace platform
