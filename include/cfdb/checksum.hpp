#pragma once

#include <cstdint>
#include <string_view>

namespace cfdb {

// CRC-32/ISO-HDLC (IEEE). Processes every byte, including embedded zeros.
[[nodiscard]] std::uint32_t crc32(std::string_view bytes);

} // namespace cfdb
