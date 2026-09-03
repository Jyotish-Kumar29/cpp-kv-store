#include "Utils.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

std::string_view next_field(std::string_view& sv) {
    const size_t pos = sv.find('|');

    const std::string_view field = (pos == std::string_view::npos) ? sv : sv.substr(0, pos);

    sv = (pos == std::string_view::npos) ? std::string_view{} : sv.substr(pos + 1);

    return field;
}

bool parse_uint64(std::string_view field, uint64_t& out, std::string& error_reason) {
    if (field.empty()) {
        error_reason = "EMPTY_FIELD";
        return false;
    }

    if (field[0] == '-') {
        error_reason = "NEGATIVE_NOT_ALLOWED";
        return false;
    }

    if (field[0] == '+') {
        error_reason = "PLUS_SIGN_NOT_ALLOWED";
        return false;
    }

    auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), out);

    if (ec == std::errc::invalid_argument) {
        error_reason = "NOT_A_NUMBER";
        return false;
    }

    if (ec == std::errc::result_out_of_range) {
        error_reason = "NUMBER_TOO_LARGE";
        return false;
    }

    if (ptr != field.data() + field.size()) {
        error_reason = "TRAILING_JUNK";
        return false;
    }

    return true;
}