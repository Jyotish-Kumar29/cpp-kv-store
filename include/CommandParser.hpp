/**
 * \file CommandParser.hpp
 * \brief Provides command parsing utilities for the KVStore server.
 */

#pragma once

#include <string>

/**
 * \brief Parses a raw client command string into its components.
 * 
 * Extracts the command type, key, and value from a whitespace-separated string.
 * It is whitespace-agnostic and case-insensitive for the command type.
 *
 * \param command The raw command string received from the client.
 * \param commandType A reference to store the parsed command type (e.g., "SET").
 * \param key A reference to store the parsed key.
 * \param value A reference to store the parsed value (if applicable).
 * \return True if the command is valid and successfully parsed, false otherwise.
 */
bool parse_command(const std::string& command, std::string& commandType, std::string& key,
                   std::string& value);