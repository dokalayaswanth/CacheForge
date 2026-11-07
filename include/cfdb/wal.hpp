#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cfdb {

enum class Operation : std::uint8_t {
    Put = 1,
    Delete = 2
};

struct WalRecord {
    Operation operation;
    std::string key;
    std::string value;
};

// Single-caller binary operation log. Appends assume an existing valid log.
// Stream flushing is not durable synchronization. No repair or checksum yet.
class Wal {
public:
    static constexpr std::size_t max_key_size = 64 * 1024;
    static constexpr std::size_t max_value_size = 1024 * 1024;

    explicit Wal(std::filesystem::path path);

    void appendPut(const std::string& key, const std::string& value);
    void appendDelete(const std::string& key);

    // Returns all records in order. Throws runtime_error on malformed data or
    // I/O failure; does not return a partial result or alter the log.
    [[nodiscard]] std::vector<WalRecord> readAll() const;

private:
    std::filesystem::path path_;
};

} // namespace cfdb
