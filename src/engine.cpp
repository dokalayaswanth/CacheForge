#include "cfdb/engine.hpp"

namespace cfdb {

void Engine::put(const std::string& key, const std::string& value) {
    memtable_.insert_or_assign(key, value);
}

std::optional<std::string> Engine::get(const std::string& key) const {
    const auto it = memtable_.find(key);
    if (it == memtable_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Engine::erase(const std::string& key) {
    return memtable_.erase(key) != 0;
}

std::size_t Engine::size() const {
    return memtable_.size();
}

} // namespace cfdb
