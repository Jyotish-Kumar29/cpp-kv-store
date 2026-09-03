#include "UserBook.hpp"

#include <mutex>

// IDs start at 1; 0 is reserved as the unauthenticated sentinel in ClientState.
UserBook::UserBook() : next_user_id(1) {}

// Returns the new user_id on success, or 0 if name or password is empty.
uint64_t UserBook::register_user(std::string user_name, std::string password) {
    if (user_name.empty() || password.empty()) {
        return 0;
    }

    std::unique_lock<std::shared_mutex> lock(user_mutex);

    const uint64_t user_id = next_user_id++;

    users_map.emplace(user_id, UserInfo(std::move(user_name), std::move(password)));

    return user_id;
}

bool UserBook::authenticate_user(uint64_t user_id, const std::string& password) {
    std::shared_lock<std::shared_mutex> lock(user_mutex);

    auto it = users_map.find(user_id);

    if (it == users_map.end()) {
        return false;
    }

    return it->second.password == password;
}

bool UserBook::exists(uint64_t user_id) {
    std::shared_lock<std::shared_mutex> lock(user_mutex);

    return users_map.find(user_id) != users_map.end();
}

std::string UserBook::get_user_name(uint64_t user_id) {
    std::shared_lock<std::shared_mutex> lock(user_mutex);

    auto it = users_map.find(user_id);

    if (it == users_map.end()) {
        return "";
    }

    return it->second.user_name;
}