#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace platform {

enum class ExpansionNodeKind {
    None,
    Spi,
    Gpio,
};

struct ExpansionBusInventory {
    std::vector<std::string> spi_nodes;
    std::vector<std::string> gpio_chips;
    std::string error;
};

struct St25r3916Identity {
    std::uint8_t value{0};
    std::uint8_t revision{0};
};

struct CapNfcProbeResult {
    bool detected{false};
    St25r3916Identity identity;
    std::string error;
};

ExpansionNodeKind classify_expansion_node(std::string_view name);
ExpansionBusInventory read_expansion_bus_inventory();
bool decode_st25r3916_identity(std::uint8_t value, St25r3916Identity& identity);
CapNfcProbeResult read_cap_nfc_identity();

} // namespace platform
