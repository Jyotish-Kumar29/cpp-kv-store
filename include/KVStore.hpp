#pragma once

#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// Thread-safe, optionally persistent key-value store backed by an
// unordered_map + shared_mutex. When persistent = true (default), every
// mutation is appended to an AOF file so the store survives restart.
// Pass false for in-memory-only use (benchmarks skip disk I/O this way).
class KVStore {
private:
    std::unordered_map<std::string, std::string> store;
    std::shared_mutex mtx;
    std::string storage_file;
    std::ofstream aof_file;
    bool persistent;

    void save_to_file(const std::string& command);
    void load_from_file();

public:
    // storage_path is ignored when persistent_ = false.
    explicit KVStore(bool persistent_ = true, std::string storage_path = "data/kvstore.aof");
    ~KVStore();
    // autoSave = false skips the AOF write — used during replay on startup
    // to avoid re-logging what is already on disk.
    void set(const std::string& key, const std::string& value, std::string& response,
             bool autoSave = true);
    // autoSave has no effect on GET; accepted for API symmetry only.
    void get(const std::string& key, std::string& response, bool autoSave = true);
    void del(const std::string& key, std::string& response, bool autoSave = true);
};