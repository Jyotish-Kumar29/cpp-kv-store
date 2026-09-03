#pragma once

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct UserInfo {
    std::string user_name;

    // Plaintext password storage is intentional for this demo project.
    // Production code should use a salted password-hashing scheme.
    std::string password;

    UserInfo(std::string user_name_, std::string password_)
        : user_name(std::move(user_name_)), password(std::move(password_)) {}
};

// Thread-safe user registry. Assigns each new user a monotonically
// increasing uint64_t ID; all KVStore data is namespaced by this ID.
class UserBook {
public:
    UserBook();

    // Returns the new user_id on success, 0 if name or password is empty.
    uint64_t register_user(std::string user_name, std::string password);
    
    bool authenticate_user(uint64_t user_id, const std::string& password);
    bool exists(uint64_t user_id);

    // Returns an empty string if user_id is not found.
    std::string get_user_name(uint64_t user_id);

private:
    std::unordered_map<uint64_t, UserInfo> users_map;
    uint64_t next_user_id;
    std::shared_mutex user_mutex;
};