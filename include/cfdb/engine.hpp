#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace cfdb {

// Single-threaded, in-memory store. Keys and values are owned by the engine.
class Engine {
public:
    void put(const std::string& key, const std::string& value);

    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    bool erase(const std::string& key);

    [[nodiscard]] std::size_t size() const;

private:
    std::map<std::string, std::string> memtable_;
};

} // namespace cfdb
