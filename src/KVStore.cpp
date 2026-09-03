#include "KVStore.hpp"

#include <filesystem>

#include "Utils.hpp"

KVStore::KVStore(bool persistent_, std::string storage_path)
    : persistent(persistent_), storage_file(std::move(storage_path)) {
    if (persistent) {
        std::filesystem::path p(storage_file);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }

        load_from_file();                            // replay before opening for append
        aof_file.open(storage_file, std::ios::app);  // append-only: never truncates existing log
        if (!aof_file.is_open()) {
            std::cerr << "Critical Error(DATA PERSISTENCE): could not open aof file for writing.\n";
        }
    }
}

KVStore::~KVStore() {
    if (aof_file.is_open()) {
        aof_file.flush();
        aof_file.close();
    }
}

void KVStore::set(const std::string& key, const std::string& value, std::string& response,
                  bool autoSave) {
    std::unique_lock<std::shared_mutex> lock(mtx);
    // Write-ahead: log to AOF before applying to the map.
    if (autoSave && persistent) save_to_file("SET|" + key + "|" + value + '\n');
    store.insert_or_assign(key, value);
    response = "OK\n";
}

void KVStore::get(const std::string& key, std::string& response, bool autoSave) {
    std::shared_lock<std::shared_mutex> lock(mtx);
    auto it = store.find(key);
    if (it != store.end()) {
        response = it->second + '\n';
    } else {
        response = "NOT_FOUND\n";
    }
}

void KVStore::del(const std::string& key, std::string& response, bool autoSave) {
    std::unique_lock<std::shared_mutex> lock(mtx);
    auto it = store.find(key);
    if (it != store.end()) {
        if (autoSave && persistent) save_to_file("DEL|" + key + '\n');
        store.erase(it);
        response = "OK\n";
    } else {
        response = "NOT_FOUND\n";
    }
}

void KVStore::save_to_file(const std::string& command) {
    if (aof_file.is_open()) {
        aof_file << command;
        aof_file.flush();  // flush per write — durability over throughput
    } else {
        std::cerr << "Error: AOF file is not open!\n";
    }
}

void KVStore::load_from_file() {
    std::ifstream inFile(storage_file);

    if (!inFile.is_open()) {
        std::cout << "AOF file not found. Starting fresh.\n";
        return;
    }

    std::string line;
    std::string response;

    std::cout << "Loading old server data from AOF file...\n";

    while (getline(inFile, line)) {
        std::string_view data = line;
        std::string_view command = next_field(data);

        if (command == "SET") {
            std::string_view key = next_field(data);
            std::string_view value = next_field(data);
            // Replay directly into store — bypasses set() to avoid re-logging
            // entries that are already on disk.
            store.insert_or_assign(std::string(key), std::string(value));
        } else if (command == "DEL") {
            std::string key = std::string(next_field(data));
            auto it = store.find(key);
            if (it != store.end()) {
                store.erase(it);
            }
        }
    }

    inFile.close();
}
