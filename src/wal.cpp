#include "cfdb/wal.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace cfdb {
namespace {

std::array<char, 4> encodeLength(std::uint32_t value) {
    return {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >> 24) & 0xFFu)
    };
}

void readExact(std::istream& input, char* destination, std::size_t size) {
    if (size == 0) {
        return;
    }
    input.read(destination, static_cast<std::streamsize>(size));
    if (input.bad()) {
        throw std::runtime_error("WAL read failed");
    }
    if (!input) {
        throw std::runtime_error("WAL record is truncated");
    }
}

std::uint32_t readLength(std::istream& input) {
    std::array<char, 4> bytes{};
    readExact(input, bytes.data(), bytes.size());
    std::uint32_t value = 0;
    for (unsigned int i = 0; i < bytes.size(); ++i) {
        // Convert through unsigned char so bytes >= 128 do not sign-extend.
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]))
                 << (8 * i);
    }
    return value;
}

void appendRecord(const std::filesystem::path& path, Operation operation,
                  const std::string& key, const std::string& value) {
    // Validate before opening, creating, or modifying the file.
    if (key.size() > Wal::max_key_size || value.size() > Wal::max_value_size) {
        throw std::invalid_argument("WAL key or value exceeds size limit");
    }

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Cannot open WAL for append");
    }
    const auto key_length = encodeLength(static_cast<std::uint32_t>(key.size()));
    const auto value_length = encodeLength(static_cast<std::uint32_t>(value.size()));
    output.put(static_cast<char>(operation));
    output.write(key_length.data(), static_cast<std::streamsize>(key_length.size()));
    output.write(value_length.data(), static_cast<std::streamsize>(value_length.size()));
    output.write(key.data(), static_cast<std::streamsize>(key.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) {
        throw std::runtime_error("WAL write failed; log may contain a partial record");
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("WAL flush failed; log may contain a partial record");
    }
    output.close();
    if (!output) {
        throw std::runtime_error("WAL close failed; log contents are uncertain");
    }
}

} // namespace

Wal::Wal(std::filesystem::path path) : path_(std::move(path)) {}

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

    std::vector<WalRecord> records;
    for (;;) {
        char operation_byte = 0;
        input.get(operation_byte);
        if (!input) {
            if (input.eof() && !input.bad()) {
                break; // EOF is valid only before the next record starts.
            }
            throw std::runtime_error("WAL read failed");
        }
        const auto operation = static_cast<Operation>(
            static_cast<unsigned char>(operation_byte));
        if (operation != Operation::Put && operation != Operation::Delete) {
            throw std::runtime_error("WAL contains an invalid operation");
        }

        const auto key_length = readLength(input);
        const auto value_length = readLength(input);
        // Validate before allocating strings or reading their contents.
        if (key_length > max_key_size || value_length > max_value_size) {
            throw std::runtime_error("WAL record exceeds size limit");
        }
        if (operation == Operation::Delete && value_length != 0) {
            throw std::runtime_error("WAL delete record contains a value");
        }
        WalRecord record{operation, std::string(key_length, '\0'),
                         std::string(value_length, '\0')};
        readExact(input, record.key.data(), record.key.size());
        readExact(input, record.value.data(), record.value.size());
        records.push_back(std::move(record));
    }
    return records;
}

} // namespace cfdb
