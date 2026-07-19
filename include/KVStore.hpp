/**
 * \file KVStore.hpp
 * \brief Defines the KVStore class for in-memory key-value storage with AOF persistence.
 */

#pragma once

#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "CommandParser.hpp"

/**
 * \class KVStore
 * \brief A thread-safe key-value store with Append-Only File (AOF) persistence.
 *
 * Provides concurrent read access and exclusive write access using a shared mutex.
 */
class KVStore {
private:
    std::unordered_map<std::string, std::string> store_;
    std::shared_mutex mtx_;
    std::string storage_file_;
    std::ofstream aof_file_;
    
    /**
     * \brief Appends a command to the AOF file for persistence.
     * \param command The command string to save (e.g., "SET key value\n").
     */
    void save_to_file(const std::string& command);

    /**
     * \brief Loads data from the AOF file on startup to restore state.
     */
    void load_from_file();

public:
    /**
     * \brief Constructs the KVStore, initializing storage and loading AOF data.
     */
    KVStore();
    
    /**
     * \brief Destructs the KVStore, ensuring AOF file is safely flushed and closed.
     */
    ~KVStore();
    
    /**
     * \brief Sets a key-value pair in the store.
     * \param key The key to set.
     * \param value The value to associate with the key.
     * \param response A reference to a string to store the response ("OK\n").
     * \param autoSave Whether to log this operation to the AOF file.
     */
    void set(const std::string& key, const std::string& value, std::string& response,
             bool autoSave = true);
             
    /**
     * \brief Gets a value from the store by key.
     * \param key The key to look up.
     * \param response A reference to a string to store the retrieved value, or "NOT FOUND\n".
     * \param autoSave (Unused for GET) Kept for signature compatibility.
     */
    void get(const std::string& key, std::string& response, bool autoSave = true);
    
    /**
     * \brief Deletes a key-value pair from the store.
     * \param key The key to delete.
     * \param response A reference to a string to store the response ("OK\n" or "NOT FOUND\n").
     * \param autoSave Whether to log this operation to the AOF file.
     */
    void del(const std::string& key, std::string& response, bool autoSave = true);
};