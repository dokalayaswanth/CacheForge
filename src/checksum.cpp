#include "cfdb/checksum.hpp"

namespace cfdb {

std::uint32_t crc32(std::string_view bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace cfdb
