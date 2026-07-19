/**
 * \file CommandParser.cpp
 * \brief Implementation of command parsing utilities.
 */

#include "CommandParser.hpp"

#include <algorithm>
#include <cctype>

bool parse_command(const std::string& command, std::string& commandType, std::string& key,
                   std::string& value) {
    commandType = "";
    key = "";
    value = "";

    // 1. Trim leading whitespace
    size_t start = command.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }

    // 2. Find end of commandType (first whitespace after start)
    size_t firstSpace = command.find_first_of(" \t\r\n", start);
    if (firstSpace == std::string::npos) {
        commandType = command.substr(start);
    } else {
        commandType = command.substr(start, firstSpace - start);
    }

    // Convert commandType to uppercase
    std::transform(commandType.begin(), commandType.end(), commandType.begin(), [](unsigned char c) {
        return std::toupper(c);
    });

    if (commandType == "SET") {
        if (firstSpace == std::string::npos) {
            return false;
        }

        // Find start of key (skip multiple spaces)
        size_t keyStart = command.find_first_not_of(" \t\r\n", firstSpace);
        if (keyStart == std::string::npos) {
            return false;
        }

        // Find end of key (first whitespace after keyStart)
        size_t keyEnd = command.find_first_of(" \t\r\n", keyStart);
        if (keyEnd == std::string::npos) {
            return false; // SET requires both key and value
        }

        key = command.substr(keyStart, keyEnd - keyStart);

        // Find start of value (skip spaces after keyEnd)
        size_t valueStart = command.find_first_not_of(" \t\r\n", keyEnd);
        if (valueStart == std::string::npos) {
            return false; // SET requires non-empty value
        }

        // Value is the rest of the string, trimmed of trailing spaces
        size_t valueEnd = command.find_last_not_of(" \t\r\n");
        value = command.substr(valueStart, valueEnd - valueStart + 1);
        return true;
    }
    else if (commandType == "GET" || commandType == "DEL") {
        if (firstSpace == std::string::npos) {
            return false;
        }

        // Find start of key
        size_t keyStart = command.find_first_not_of(" \t\r\n", firstSpace);
        if (keyStart == std::string::npos) {
            return false;
        }

        // The key is the remainder of the command, trimmed of trailing spaces
        size_t keyEnd = command.find_last_not_of(" \t\r\n");
        key = command.substr(keyStart, keyEnd - keyStart + 1);

        // Ensure there are no spaces within the key itself for GET/DEL
        if (key.find_first_of(" \t\r\n") != std::string::npos) {
            return false;
        }

        return true;
    }

    return false;
}