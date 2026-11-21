#include "cfdb/engine.hpp"

#include <stdexcept>
#include <utility>

namespace cfdb {

Engine::Engine(std::filesystem::path wal_path)
    : wal_(std::filesystem::absolute(std::move(wal_path))) {
    wal_->ensureExists();
    // readAll validates the entire log before replay begins. Apply directly:
    // public mutation methods would append duplicate records during recovery.
    for (const auto& record : wal_->readAll()) {
        if (record.operation == Operation::Put) {
            memtable_.insert_or_assign(record.key, record.value);
        } else {
            memtable_.erase(record.key);
        }
    }
}

void Engine::requireHealthy() const {
    if (failed_) {
        throw std::runtime_error("Engine has failed; close it and reopen after inspecting the WAL");
    }
}

void Engine::put(const std::string& key, const std::string& value) {
    requireHealthy();
    if (!wal_) {
        memtable_.insert_or_assign(key, value);
        return;
    }
    // Caller mistakes must not poison the engine or touch either storage layer.
    if (key.size() > Wal::max_key_size || value.size() > Wal::max_value_size) {
        throw std::invalid_argument("WAL key or value exceeds size limit");
    }
    try {
        wal_->appendPut(key, value);
        memtable_.insert_or_assign(key, value);
    } catch (...) {
        // The WAL may be partial, or the append may have succeeded before a
        // memory allocation failed. Do not serve potentially divergent state.
        failed_ = true;
        throw;
    }
}

std::optional<std::string> Engine::get(const std::string& key) const {
    requireHealthy();
    const auto it = memtable_.find(key);
    if (it == memtable_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Engine::erase(const std::string& key) {
    requireHealthy();
    if (wal_ && key.size() > Wal::max_key_size) {
        throw std::invalid_argument("WAL key exceeds size limit");
    }
    const auto it = memtable_.find(key);
    if (it == memtable_.end()) {
        return false;
    }
    if (wal_) {
        try {
            wal_->appendDelete(key);
            memtable_.erase(it);
        } catch (...) {
            failed_ = true;
            throw;
        }
    } else {
        memtable_.erase(it);
    }
    return true;
}

std::size_t Engine::size() const {
    requireHealthy();
    return memtable_.size();
}

} // namespace cfdb
