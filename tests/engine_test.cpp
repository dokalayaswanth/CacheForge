#include "cfdb/engine.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

// Unlike assert(), these checks also run in Release builds.
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void empty_store() {
    const cfdb::Engine engine;
    require(engine.size() == 0, "new engine must be empty");
    require(!engine.get("missing"), "missing key must return nullopt");
    require(engine.size() == 0, "lookup must not insert a key");
}

void insert_and_update() {
    cfdb::Engine engine;
    engine.put("city", "Seattle");
    require(engine.get("city") == "Seattle", "insert must be retrievable");
    require(engine.size() == 1, "insert must increase size");
    engine.put("city", "Boston");
    require(engine.get("city") == "Boston", "put must replace existing value");
    require(engine.size() == 1, "update must not increase size");
    require(!engine.get("missing"), "missing key in populated engine");
    require(engine.size() == 1, "missing lookup must not change size");
}

void deletion() {
    cfdb::Engine engine;
    engine.put("a", "one");
    engine.put("b", "two");
    require(engine.erase("a"), "existing key must be removed");
    require(!engine.get("a"), "removed key must be absent");
    require(!engine.erase("a"), "second deletion must return false");
    require(engine.size() == 1, "missing deletion must not change size");
    require(engine.get("b") == "two", "deletion must preserve other keys");
    engine.put("a", "three");
    require(engine.get("a") == "three", "deleted key can be reinserted");
}

void empty_keys_and_values() {
    cfdb::Engine engine;
    engine.put("nickname", "");
    const auto value = engine.get("nickname");
    require(value.has_value(), "empty value must be present");
    require(value->empty(), "empty value must remain empty");
    require(!engine.get("unknown"), "missing and empty must differ");
    engine.put("", "empty-key-value");
    require(engine.get("") == "empty-key-value", "empty key must work");
    engine.put("", "");
    require(engine.get("") == "", "empty key can store an empty value");
    require(engine.size() == 2, "empty-key update must preserve size");
    require(engine.erase(""), "empty key must be removable");
}

void case_sensitive_keys() {
    cfdb::Engine engine;
    engine.put("Name", "upper");
    engine.put("name", "lower");
    require(engine.size() == 2, "keys must be case sensitive");
    require(engine.get("Name") == "upper", "uppercase key value");
    require(engine.get("name") == "lower", "lowercase key value");
}

void owned_inputs_and_results() {
    cfdb::Engine engine;
    {
        std::string key = "city";
        std::string value = "Seattle";
        engine.put(key, value);
        key = "country";
        value = "Boston";
        require(engine.get("city") == "Seattle", "inputs must be copied");
        require(!engine.get("country"), "changing input must not rename key");
    }
    require(engine.get("city") == "Seattle", "data must outlive input strings");
    auto result = engine.get("city");
    require(result.has_value(), "stored value must exist");
    *result = "Portland";
    require(engine.get("city") == "Seattle", "get must return an independent copy");
}

void independent_engines() {
    cfdb::Engine first;
    cfdb::Engine second;
    first.put("key", "first");
    require(!second.get("key"), "instances must not share storage");
    second.put("key", "second");
    require(first.get("key") == "first", "second engine must not modify first");
    require(second.get("key") == "second", "second engine owns its value");
}

} // namespace

int main() {
    struct Test {
        const char* name;
        void (*run)();
    };
    const Test tests[] = {
        {"empty store", empty_store},
        {"insert and update", insert_and_update},
        {"deletion", deletion},
        {"empty keys and values", empty_keys_and_values},
        {"case sensitive keys", case_sensitive_keys},
        {"owned inputs and results", owned_inputs_and_results},
        {"independent engines", independent_engines},
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
