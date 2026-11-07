#include "cfdb/wal.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void requireThrows(Function function, const char* message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

// Own only a freshly created directory and its one log file; never reuse or
// recursively delete another test's directory.
class TempLog {
public:
    TempLog() {
        std::random_device random;
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            directory_ = base / ("cfdb-wal-test-" + std::to_string(random()) +
                                  "-" + std::to_string(random()));
            if (std::filesystem::create_directory(directory_)) {
                return;
            }
        }
        throw std::runtime_error("Cannot create unique test directory");
    }

    ~TempLog() {
        std::error_code ignored;
        std::filesystem::remove(path(), ignored);
        std::filesystem::remove(directory_, ignored);
    }

    TempLog(const TempLog&) = delete;
    TempLog& operator=(const TempLog&) = delete;

    std::filesystem::path path() const { return directory_ / "test.wal"; }

private:
    std::filesystem::path directory_;
};

std::string bytes(std::initializer_list<unsigned int> values) {
    std::string result;
    for (auto value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void writeFixture(const std::filesystem::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "fixture open failed");
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    require(static_cast<bool>(output), "fixture write failed");
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture read open failed");
    std::string result{std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>()};
    require(!input.bad(), "fixture read failed");
    return result;
}

void ordered_records_and_reopen() {
    TempLog file;
    {
        cfdb::Wal wal(file.path());
        require(!std::filesystem::exists(file.path()), "constructor must not create file");
        wal.appendPut("city", "Seattle");
        wal.appendPut("language", "CPP");
        wal.appendPut("city", "Boston");
        wal.appendDelete("language");
    }
    cfdb::Wal reopened(file.path());
    const auto records = reopened.readAll();
    require(records.size() == 4, "must preserve all operations including old versions");
    require(records[0].operation == cfdb::Operation::Put &&
                records[0].key == "city" && records[0].value == "Seattle", "first put");
    require(records[1].operation == cfdb::Operation::Put &&
                records[1].key == "language" && records[1].value == "CPP", "second put");
    require(records[2].operation == cfdb::Operation::Put &&
                records[2].key == "city" && records[2].value == "Boston", "update order");
    require(records[3].operation == cfdb::Operation::Delete &&
                records[3].key == "language" && records[3].value.empty(), "delete record");
}

void append_preserves_prefix() {
    TempLog file;
    cfdb::Wal first(file.path());
    first.appendPut("first", "value");
    const auto before = readBytes(file.path());
    cfdb::Wal second(file.path());
    second.appendPut("second", "next");
    const auto after = readBytes(file.path());
    require(after.size() > before.size(), "append must grow file");
    require(after.substr(0, before.size()) == before, "append must preserve existing bytes");
    require(first.readAll().size() == 2, "each read sees newly appended records");
}

void binary_and_empty_strings() {
    TempLog file;
    cfdb::Wal wal(file.path());
    const std::string key("k\0x", 3);
    const std::string value = bytes({0, 0x80, 0xFF, 10, 13});
    wal.appendPut(key, value);
    wal.appendPut("", "");
    wal.appendPut("full name", "New York");
    wal.appendDelete(key);
    wal.appendDelete("");
    const auto records = wal.readAll();
    require(records.size() == 5, "all binary records must be retained");
    require(records[0].key == key && records[0].value == value, "raw bytes must survive");
    require(records[1].key.empty() && records[1].value.empty(), "empty put must survive");
    require(records[2].key == "full name" && records[2].value == "New York", "spaces must survive");
    require(records[3].key == key && records[3].operation == cfdb::Operation::Delete,
            "delete must preserve binary key");
    require(records[4].key.empty() && records[4].operation == cfdb::Operation::Delete,
            "empty delete must survive");
}

void golden_bytes() {
    TempLog file;
    cfdb::Wal wal(file.path());
    wal.appendPut("city", "Boston");
    wal.appendDelete("city");
    const auto expected = bytes({1, 4, 0, 0, 0, 6, 0, 0, 0}) + "cityBoston" +
                          bytes({2, 4, 0, 0, 0, 0, 0, 0, 0}) + "city";
    require(readBytes(file.path()) == expected, "on-disk bytes must match format");

    // Independent reader fixture: both multi-byte and high-bit length bytes.
    const std::string key(258, 'k');
    const std::string value(255, 'v');
    const auto fixture = bytes({1, 2, 1, 0, 0, 255, 0, 0, 0}) + key + value;
    writeFixture(file.path(), fixture);
    const auto records = wal.readAll();
    require(records.size() == 1 && records[0].key == key && records[0].value == value,
            "reader must decode little-endian and unsigned length bytes");
    writeFixture(file.path(), "");
    wal.appendPut(key, value);
    require(readBytes(file.path()) == fixture, "writer must encode multi-byte lengths");
}

void empty_and_missing_files() {
    TempLog file;
    cfdb::Wal wal(file.path());
    requireThrows<std::runtime_error>([&] { (void)wal.readAll(); }, "missing file must fail");
    require(!std::filesystem::exists(file.path()), "read must not create file");
    writeFixture(file.path(), "");
    require(wal.readAll().empty(), "empty file must succeed");
}

void truncated_records() {
    TempLog file;
    cfdb::Wal wal(file.path());
    const auto put = bytes({1, 3, 0, 0, 0, 5, 0, 0, 0}) + "keyvalue";
    const auto deletion = bytes({2, 3, 0, 0, 0, 0, 0, 0, 0}) + "key";
    for (const auto& record : {put, deletion}) {
        for (std::size_t length = 1; length < record.size(); ++length) {
            // Test every cut, both as the first record and after a valid prefix.
            for (const auto& prefix : {std::string{}, put}) {
                const auto damaged = prefix + record.substr(0, length);
                writeFixture(file.path(), damaged);
                requireThrows<std::runtime_error>([&] { (void)wal.readAll(); },
                                                  "partial record must fail");
                require(readBytes(file.path()) == damaged, "reader must not repair damaged file");
            }
        }
    }
    writeFixture(file.path(), put);
    require(wal.readAll().size() == 1, "EOF at record boundary must succeed");
}

void invalid_operations() {
    TempLog file;
    cfdb::Wal wal(file.path());
    for (unsigned int operation : {0u, 3u, 128u, 255u}) {
        writeFixture(file.path(), bytes({operation, 0, 0, 0, 0, 0, 0, 0, 0}));
        requireThrows<std::runtime_error>([&] { (void)wal.readAll(); }, "invalid operation must fail");
    }
}

void invalid_delete_value() {
    TempLog file;
    cfdb::Wal wal(file.path());
    writeFixture(file.path(), bytes({2, 1, 0, 0, 0, 1, 0, 0, 0}) + "kv");
    requireThrows<std::runtime_error>([&] { (void)wal.readAll(); }, "delete value must be rejected");
}

void invalid_encoded_lengths() {
    TempLog file;
    cfdb::Wal wal(file.path());
    // No payload: each malformed length must be rejected before allocation.
    const std::string headers[] = {
        bytes({1, 1, 0, 1, 0, 0, 0, 0, 0}), // key = 65537
        bytes({1, 0, 0, 0, 0, 1, 0, 16, 0}), // value = 1048577
        bytes({1, 255, 255, 255, 255, 0, 0, 0, 0}),
        bytes({1, 0, 0, 0, 0, 255, 255, 255, 255}),
        bytes({2, 255, 255, 255, 255, 0, 0, 0, 0})
    };
    for (const auto& header : headers) {
        writeFixture(file.path(), header);
        try {
            (void)wal.readAll();
            throw std::logic_error("oversized encoded length was accepted");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()).find("size limit") != std::string::npos,
                    "oversized length must fail validation, not payload reading");
        }
    }
}

void exact_size_limits() {
    TempLog file;
    cfdb::Wal wal(file.path());
    const std::string key(64 * 1024, 'k');
    const std::string value(1024 * 1024, 'v');
    wal.appendPut(key, value);
    wal.appendDelete(key);
    const auto records = wal.readAll();
    require(records.size() == 2 && records[0].key == key && records[0].value == value,
            "exact size limits must be accepted");
    require(records[1].key == key && records[1].value.empty(), "maximum delete key");
}

void oversized_input_preserves_file() {
    TempLog file;
    cfdb::Wal wal(file.path());
    const std::string large_key(64 * 1024 + 1, 'k');
    const std::string large_value(1024 * 1024 + 1, 'v');
    requireThrows<std::invalid_argument>([&] { wal.appendPut(large_key, ""); }, "oversized key");
    requireThrows<std::invalid_argument>([&] { wal.appendPut("", large_value); }, "oversized value");
    requireThrows<std::invalid_argument>([&] { wal.appendDelete(large_key); }, "oversized delete key");
    require(!std::filesystem::exists(file.path()), "invalid input must not create file");
    wal.appendPut("existing", "value");
    const auto before = readBytes(file.path());
    requireThrows<std::invalid_argument>([&] { wal.appendPut(large_key, ""); }, "oversized key");
    requireThrows<std::invalid_argument>([&] { wal.appendPut("", large_value); }, "oversized value");
    requireThrows<std::invalid_argument>([&] { wal.appendDelete(large_key); }, "oversized delete key");
    require(readBytes(file.path()) == before, "invalid input must not modify existing bytes");
}

void append_open_failure() {
    TempLog file;
    // test.wal does not exist as a directory, so this child path cannot open.
    cfdb::Wal wal(file.path() / "missing-parent.wal");
    requireThrows<std::runtime_error>([&] { wal.appendPut("key", "value"); }, "open failure must throw");
    requireThrows<std::runtime_error>([&] { wal.appendDelete("key"); }, "delete open failure must throw");
}

} // namespace

int main() {
    struct Test {
        const char* name;
        void (*run)();
    };
    const Test tests[] = {
        {"ordered records and reopen", ordered_records_and_reopen},
        {"append preserves prefix", append_preserves_prefix},
        {"binary and empty strings", binary_and_empty_strings},
        {"golden bytes", golden_bytes},
        {"empty and missing files", empty_and_missing_files},
        {"truncated records", truncated_records},
        {"invalid operations", invalid_operations},
        {"invalid delete value", invalid_delete_value},
        {"invalid encoded lengths", invalid_encoded_lengths},
        {"exact size limits", exact_size_limits},
        {"oversized input preserves file", oversized_input_preserves_file},
        {"append open failure", append_open_failure}
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
