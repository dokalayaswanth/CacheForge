#include "cfdb/checksum.hpp"

#include <iostream>
#include <string_view>

int main() {
    struct Case {
        const char* name;
        std::string_view input;
        std::uint32_t expected;
    };
    // Independent expected values: standard check vector and Python zlib.crc32.
    const char binary[] = {0, static_cast<char>(0x80), static_cast<char>(0xFF), 0, 10};
    const Case cases[] = {
        {"empty input", {}, 0u},
        {"standard check vector", "123456789", 0xCBF43926u},
        {"binary bytes including zero", {binary, sizeof(binary)}, 0x75369cd5u}
    };
    int failures = 0;
    for (const auto& test : cases) {
        if (cfdb::crc32(test.input) != test.expected) {
            ++failures;
            std::cerr << "FAIL: " << test.name << '\n';
        } else {
            std::cout << "PASS: " << test.name << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
