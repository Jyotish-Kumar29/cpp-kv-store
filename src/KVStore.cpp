/**
 * \file KVStore.cpp
 * \brief Implementation of the KVStore class.
 */

#include "KVStore.hpp"
#include <filesystem>

KVStore::KVStore() {
    std::filesystem::create_directories("data");
    load_from_file();

    aof_file_.open("data/kvstore.aof", std::ios::app);
    if (!aof_file_.is_open()) {
        std::cerr << "Critical Error(DATA PERSISTENCE): could not open aof file for writing.\n";
    }
}

KVStore::~KVStore() {
    if (aof_file_.is_open()) {
        aof_file_.flush();
        aof_file_.close();
    }
}

void KVStore::set(const std::string& key, const std::string& value, std::string& response,
                  bool autoSave) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    if (autoSave) save_to_file("SET " + key + " " + value + '\n');
    store_.insert_or_assign(key, value);
    response = "OK\n";
}

void KVStore::get(const std::string& key, std::string& response, bool autoSave) {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        response = it->second + '\n';
    } else {
        response = "NOT FOUND\n";
    }
}

void KVStore::del(const std::string& key, std::string& response, bool autoSave) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        if (autoSave) save_to_file("DEL " + key + '\n');
        store_.erase(it);
        response = "OK\n";
    } else {
        response = "NOT FOUND\n";
    }
}

void KVStore::save_to_file(const std::string& command) {
    if (aof_file_.is_open()) {
        aof_file_ << command;
        aof_file_.flush();
    } else {
        std::cerr << "Error: AOF file is not open!\n";
    }
}

void KVStore::load_from_file() {
    std::ifstream inFile("data/kvstore.aof");

    if (!inFile.is_open()) {
        std::cout << "AOF file not found. Starting fresh.\n";
        return;
    }

    std::string response;
    std::string commandData;
    std::string commandType, key, value;

    std::cout << "Loading old server data from AOF file...\n";  

    while (getline(inFile, commandData)) {
        if (parse_command(commandData, commandType, key, value)) {
            if (commandType == "SET") {
                set(key, value, response, false);
            } else if (commandType == "GET") {
                get(key, response, false);
            } else if (commandType == "DEL") {
                del(key, response, false);
            }
        }
    }

    inFile.close();
}
