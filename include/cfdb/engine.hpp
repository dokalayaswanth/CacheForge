#pragma once

#include "cfdb/wal.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace cfdb {

// Single-threaded store with optional WAL recovery. One engine per WAL path;
// callers must enforce exclusive ownership (there is no process lock yet).
class Engine {
public:
    Engine() = default;
    explicit Engine(std::filesystem::path wal_path);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void put(const std::string& key, const std::string& value);

    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    bool erase(const std::string& key);

    [[nodiscard]] std::size_t size() const;

private:
    void requireHealthy() const;

    std::map<std::string, std::string> memtable_;
    std::optional<Wal> wal_;
    bool failed_ = false;
};

} // namespace cfdb
