#include "cfdb/wal.hpp"
#include "cfdb/checksum.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cfdb {
namespace {

constexpr std::array<char, 8> file_header{'C', 'F', 'W', 'L', 1, 0, 0, 0};

std::array<char, 4> encode32(std::uint32_t value) {
    return {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >> 24) & 0xFFu)
    };
}

std::uint32_t decode32(const char* bytes) {
    std::uint32_t value = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
    }
    return value;
}

[[noreturn]] void recordError(const char* reason, std::uint64_t offset) {
    throw std::runtime_error(std::string(reason) + " at byte offset " + std::to_string(offset));
}

void readExact(std::istream& input, char* destination, std::size_t size,
               std::uint64_t record_offset) {
    if (size == 0) {
        return;
    }
    input.read(destination, static_cast<std::streamsize>(size));
    if (input.bad()) {
        recordError("WAL read failed", record_offset);
    }
    if (!input) {
        recordError("WAL record is truncated", record_offset);
    }
}

void validateHeader(std::istream& input) {
    std::array<char, 8> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (input.bad()) {
        throw std::runtime_error("WAL header read failed at byte offset 0");
    }
    if (!input) {
        throw std::runtime_error("WAL header missing or truncated at byte offset 0");
    }
    if (std::string_view(header.data(), 4) != "CFWL") {
        throw std::runtime_error("Invalid WAL magic or unsupported legacy format at byte offset 0");
    }
    if (decode32(header.data() + 4) != 1) {
        throw std::runtime_error("Unsupported WAL version at byte offset 4");
    }
}

void finishWrite(std::ofstream& output) {
    if (!output) {
        throw std::runtime_error("WAL write failed; log may be incomplete");
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("WAL flush failed; log may be incomplete");
    }
    output.close();
    if (!output) {
        throw std::runtime_error("WAL close failed; log contents are uncertain");
    }
}

void initializeOrValidate(const std::filesystem::path& path) {
    // Single-caller contract: no concurrent file replacement or initialization.
    // Never interpret failure to read an existing file as permission to recreate it.
    if (std::filesystem::exists(path)) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Cannot open WAL for header validation");
        }
        if (input.peek() != std::char_traits<char>::eof()) {
            validateHeader(input);
            return;
        }
        if (input.bad() || !input.eof()) {
            throw std::runtime_error("Cannot read WAL header");
        }
    }
    // Missing or zero-byte file only. Append mode never truncates existing data.
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Cannot open or create WAL");
    }
    output.write(file_header.data(), static_cast<std::streamsize>(file_header.size()));
    finishWrite(output);
}

void appendRecord(const std::filesystem::path& path, Operation operation,
                  const std::string& key, const std::string& value) {
    // Reject caller errors before even initializing an empty file.
    if (key.size() > Wal::max_key_size || value.size() > Wal::max_value_size) {
        throw std::invalid_argument("WAL key or value exceeds size limit");
    }
    const auto key_length = encode32(static_cast<std::uint32_t>(key.size()));
    const auto value_length = encode32(static_cast<std::uint32_t>(value.size()));
    std::string body;
    body.reserve(9 + key.size() + value.size());
    body.push_back(static_cast<char>(operation));
    body.append(key_length.data(), key_length.size());
    body.append(value_length.data(), value_length.size());
    body.append(key);
    body.append(value);
    const auto checksum = encode32(crc32(body));

    initializeOrValidate(path); // Check the header on every append.
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Cannot open WAL for append");
    }
    output.write(checksum.data(), static_cast<std::streamsize>(checksum.size()));
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    finishWrite(output);
}

} // namespace

Wal::Wal(std::filesystem::path path) : path_(std::move(path)) {}

void Wal::ensureExists() {
    initializeOrValidate(path_);
}

void Wal::appendPut(const std::string& key, const std::string& value) {
    appendRecord(path_, Operation::Put, key, value);
}

void Wal::appendDelete(const std::string& key) {
    appendRecord(path_, Operation::Delete, key, "");
}

std::vector<WalRecord> Wal::readAll() const {
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open WAL for reading");
    }
    validateHeader(input);
    std::vector<WalRecord> records;
    std::uint64_t offset = file_header.size();
    for (;;) {
        std::array<char, 4> checksum{};
        input.get(checksum[0]);
        if (!input) {
            if (input.eof() && !input.bad()) {
                break; // Only EOF before a new record is a valid boundary.
            }
            recordError("WAL read failed", offset);
        }
        readExact(input, checksum.data() + 1, 3, offset);
        std::array<char, 9> metadata{};
        readExact(input, metadata.data(), metadata.size(), offset);
        const auto operation = static_cast<Operation>(static_cast<unsigned char>(metadata[0]));
        if (operation != Operation::Put && operation != Operation::Delete) {
            recordError("WAL contains an invalid operation", offset);
        }
        const auto key_length = decode32(metadata.data() + 1);
        const auto value_length = decode32(metadata.data() + 5);
        // Untrusted lengths must be bounded before allocating the record body.
        if (key_length > max_key_size || value_length > max_value_size) {
            recordError("WAL record exceeds size limit", offset);
        }
        if (operation == Operation::Delete && value_length != 0) {
            recordError("WAL delete record contains a value", offset);
        }
        std::string body(metadata.data(), metadata.size());
        body.resize(metadata.size() + key_length + value_length);
        readExact(input, body.data() + metadata.size(), key_length + value_length, offset);
        if (crc32(body) != decode32(checksum.data())) {
            recordError("WAL checksum mismatch", offset);
        }
        records.push_back({operation, body.substr(9, key_length),
                          body.substr(9 + key_length, value_length)});
        offset += checksum.size() + body.size();
    }
    return records;
}

} // namespace cfdb
