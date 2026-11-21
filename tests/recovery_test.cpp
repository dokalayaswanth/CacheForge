#include "cfdb/engine.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

namespace {

static_assert(!std::is_copy_constructible_v<cfdb::Engine>);
static_assert(!std::is_copy_assignable_v<cfdb::Engine>);
static_assert(!std::is_move_constructible_v<cfdb::Engine>);
static_assert(!std::is_move_assignable_v<cfdb::Engine>);

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

class TempDatabase {
public:
    TempDatabase() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            directory_ = std::filesystem::temp_directory_path() /
                ("cfdb-recovery-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(directory_)) {
                return;
            }
        }
        throw std::runtime_error("Cannot create test directory");
    }

    ~TempDatabase() {
        // Only remove these known paths in our freshly created directory.
        std::error_code ignored;
        std::filesystem::remove(path(), ignored);
        std::filesystem::remove(backup(), ignored);
        std::filesystem::remove(directory_, ignored);
    }

    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;
    std::filesystem::path path() const { return directory_ / "database.wal"; }
    std::filesystem::path backup() const { return directory_ / "saved.wal"; }

private:
    std::filesystem::path directory_;
};

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "cannot read fixture");
    std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(!input.bad(), "fixture read failed");
    return data;
}

void writeBytes(const std::filesystem::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "cannot open fixture");
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    require(static_cast<bool>(output), "fixture write failed");
}

void creates_empty_log() {
    TempDatabase file;
    require(!std::filesystem::exists(file.path()), "fixture must start missing");
    cfdb::Engine engine(file.path());
    require(engine.size() == 0, "new engine must be empty");
    require(std::filesystem::exists(file.path()), "engine must create log");
    require(readBytes(file.path()) == std::string("CFWL\x01\x00\x00\x00", 8), "new log must contain only its header");
}

void restart_recovers_updates_and_deletes() {
    TempDatabase file;
    {
        cfdb::Engine engine(file.path());
        engine.put("city", "Seattle");
        engine.put("city", "Boston");
        engine.put("language", "CPP");
        require(engine.erase("language"), "delete must succeed");
    }
    cfdb::Engine recovered(file.path());
    require(recovered.get("city") == "Boston", "latest put must win on restart");
    require(!recovered.get("language"), "deletion must survive restart");
    require(recovered.size() == 1, "size must count keys, not operations");
}

void binary_data_recovers() {
    TempDatabase file;
    const std::string key("a\0b", 3);
    const std::string value("x\0y\n", 4);
    {
        cfdb::Engine engine(file.path());
        engine.put("", "empty key");
        engine.put("empty value", "");
        engine.put(key, value);
    }
    cfdb::Engine recovered(file.path());
    require(recovered.get("") == "empty key", "empty key must recover");
    require(recovered.get("empty value") == "", "empty value must recover");
    require(recovered.get(key) == value, "binary strings must recover");
    require(recovered.size() == 3, "three keys must recover");
}

void replay_never_appends() {
    TempDatabase file;
    // Build a log independently of the engine, including a missing-key delete.
    cfdb::Wal wal(file.path());
    wal.appendPut("key", "old");
    wal.appendDelete("key");
    wal.appendDelete("absent");
    wal.appendPut("key", "new");
    const auto before = readBytes(file.path());
    for (int i = 0; i < 3; ++i) {
        cfdb::Engine engine(file.path());
        require(engine.get("key") == "new" && engine.size() == 1, "replay ordering");
        require(!engine.get("missing"), "missing lookup");
        require(!engine.erase("missing"), "missing delete must be a no-op");
        require(readBytes(file.path()) == before, "reopen, reads, and no-op deletes must not append");
    }
}

void writes_after_reopening() {
    TempDatabase file;
    {
        cfdb::Engine engine(file.path());
        engine.put("old", "preserved");
    }
    const auto prefix = readBytes(file.path());
    {
        cfdb::Engine engine(file.path());
        engine.put("new", "added");
        engine.put("old", "updated");
        engine.erase("new");
    }
    require(readBytes(file.path()).substr(0, prefix.size()) == prefix, "earlier records must survive");
    cfdb::Engine recovered(file.path());
    require(recovered.get("old") == "updated" && !recovered.get("new"), "new operations must recover");
}

void rejects_bad_logs_without_changes() {
    TempDatabase file;
    cfdb::Wal wal(file.path());
    wal.appendPut("key", "value");
    const auto valid = readBytes(file.path());
    const std::string invalid_operation("\x00\x00\x00\x00\x63\x00\x00\x00\x00\x00\x00\x00\x00", 13);
    const std::string invalid_delete("\x00\x00\x00\x00\x02\x00\x00\x00\x00\x01\x00\x00\x00v", 14);
    for (const auto& damaged : {valid + invalid_operation, valid + invalid_delete,
                                valid.substr(0, valid.size() - 1)}) {
        writeBytes(file.path(), damaged);
        requireThrows<std::runtime_error>([&] { cfdb::Engine engine(file.path()); },
                                          "bad log must reject construction");
        require(readBytes(file.path()) == damaged, "constructor must not repair or truncate");
    }
}

void oversized_requests_keep_engine_healthy() {
    TempDatabase file;
    {
        cfdb::Engine engine(file.path());
        engine.put("existing", "old");
        const auto before = readBytes(file.path());
        const std::string large_key(cfdb::Wal::max_key_size + 1, 'k');
        const std::string large_value(cfdb::Wal::max_value_size + 1, 'v');
        requireThrows<std::invalid_argument>([&] { engine.put(large_key, "v"); }, "oversized key");
        requireThrows<std::invalid_argument>([&] { engine.put("existing", large_value); }, "oversized value");
        requireThrows<std::invalid_argument>([&] { engine.erase(large_key); }, "oversized delete key");
        require(engine.size() == 1 && engine.get("existing") == "old", "memory must remain unchanged");
        require(readBytes(file.path()) == before, "log must remain unchanged");
        engine.put("next", "accepted");
    }
    cfdb::Engine recovered(file.path());
    require(recovered.get("next") == "accepted", "valid write after rejection must recover");
}

void failed_engine_rejects_every_operation(cfdb::Engine& engine) {
    requireThrows<std::runtime_error>([&] { (void)engine.get("key"); }, "failed get must throw");
    requireThrows<std::runtime_error>([&] { (void)engine.size(); }, "failed size must throw");
    requireThrows<std::runtime_error>([&] { engine.put("next", "v"); }, "failed put must throw");
    requireThrows<std::runtime_error>([&] { engine.erase("key"); }, "failed erase must throw");
    requireThrows<std::runtime_error>([&] { engine.erase("absent"); }, "failed no-op erase must throw");
}

void append_failure_poisoning() {
    // Exercise both public mutation paths. Restore the path before checking
    // poisoning so a missing failed-state check cannot hide behind an I/O error.
    for (bool delete_operation : {false, true}) {
        TempDatabase file;
        {
            cfdb::Engine engine(file.path());
            engine.put("key", "original");
            const auto before = readBytes(file.path());
            std::filesystem::rename(file.path(), file.backup());
            require(std::filesystem::create_directory(file.path()), "create failure fixture");
            requireThrows<std::runtime_error>([&] {
                if (delete_operation) {
                    engine.erase("key");
                } else {
                    engine.put("key", "replacement");
                }
            }, "append-open failure must propagate");
            std::filesystem::remove(file.path()); // Our empty replacement directory.
            std::filesystem::rename(file.backup(), file.path());
            failed_engine_rejects_every_operation(engine);
            require(readBytes(file.path()) == before, "failed engine must not write after path restoration");
        }
        cfdb::Engine recovered(file.path());
        require(recovered.get("key") == "original", "failed append must not be recovered as success");
        recovered.put("next", "healthy again");
    }
}

void constructor_io_errors() {
    TempDatabase file;
    requireThrows<std::runtime_error>([&] { cfdb::Engine engine(file.path() / "child.wal"); },
                                      "missing parent must fail");
    require(!std::filesystem::exists(file.path()), "constructor must not create parent directories");
    require(std::filesystem::create_directory(file.path()), "create directory fixture");
    requireThrows<std::runtime_error>([&] { cfdb::Engine engine(file.path()); },
                                      "directory is not a WAL");
}

void in_memory_mode_keeps_existing_contract() {
    cfdb::Engine engine;
    const std::string key(cfdb::Wal::max_key_size + 1, 'k');
    const std::string value(cfdb::Wal::max_value_size + 1, 'v');
    engine.put(key, value);
    require(engine.get(key) == value, "WAL limits must not change in-memory mode");
    require(engine.erase(key) && engine.size() == 0, "in-memory delete");
}


void corrupted_and_legacy_logs_reject_recovery() {
    TempDatabase file;
    {
        cfdb::Engine engine(file.path());
        engine.put("city", "Boston");
        engine.put("other", "value");
    }
    const auto valid = readBytes(file.path());
    auto corrupt_key = valid;
    corrupt_key.at(21) ^= 1;
    auto corrupt_value = valid;
    corrupt_value.at(25) ^= 1;
    auto unsupported = valid;
    unsupported.at(4) = 2;
    const std::string legacy("\x01\x01\x00\x00\x00\x01\x00\x00\x00kv", 11);
    for (const auto& data : {corrupt_key, corrupt_value, unsupported, legacy,
                             valid.substr(0, 7)}) {
        writeBytes(file.path(), data);
        requireThrows<std::runtime_error>([&] { cfdb::Engine engine(file.path()); },
                                          "damaged or unsupported log must reject engine construction");
        require(readBytes(file.path()) == data, "rejected recovery must not alter log");
    }
}

} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"creates empty log", creates_empty_log},
        {"restart recovers updates and deletes", restart_recovers_updates_and_deletes},
        {"binary data recovers", binary_data_recovers},
        {"replay never appends", replay_never_appends},
        {"writes after reopening", writes_after_reopening},
        {"rejects bad logs without changes", rejects_bad_logs_without_changes},
        {"oversized requests keep engine healthy", oversized_requests_keep_engine_healthy},
        {"append failure poisoning", append_failure_poisoning},
        {"constructor I/O errors", constructor_io_errors},
        {"in-memory mode keeps existing contract", in_memory_mode_keeps_existing_contract},
        {"corrupted and legacy logs reject recovery", corrupted_and_legacy_logs_reject_recovery}
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
