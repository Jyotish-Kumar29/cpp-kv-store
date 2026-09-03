#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Consumes and returns the next '|'-delimited field from sv, advancing sv
// past the delimiter. Returns the remainder as-is if no '|' is found.
std::string_view next_field(std::string_view& sv);

// Parses a decimal uint64_t from field. Populates error_reason on failure.
bool parse_uint64(std::string_view field, uint64_t& out, std::string& error_reason);