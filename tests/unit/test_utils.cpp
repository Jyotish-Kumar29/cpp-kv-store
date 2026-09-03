// tests/unit/test_utils.cpp
// -----------------------------------------------------------------------------
// Unit tests for the protocol-parsing helpers in Utils: next_field() (pipe-
// delimited field splitting) and parse_uint64() (strict numeric parsing
// with explicit rejection reasons).
//
// NOTE: parse_command (the old space-delimited SET/GET/DEL parser) is no
// longer called anywhere in TCPServer.cpp -- the live server uses
// next_field()/parse_uint64() on a pipe-delimited protocol instead. These
// tests cover what's actually in the request path today.
// -----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "Utils.hpp"

TEST(UtilsTest, NextFieldBasicSplit) {
    std::string_view sv = "SET|my_key|my_val";

    EXPECT_EQ(next_field(sv), "SET");
    EXPECT_EQ(next_field(sv), "my_key");
    EXPECT_EQ(next_field(sv), "my_val");  // last field, no trailing '|'
}

TEST(UtilsTest, NextFieldEmptyField) {
    // Consecutive delimiters ("||") must yield an empty field rather than
    // being collapsed or skipped, so downstream validation can reject it.
    std::string_view sv = "SET||val";

    EXPECT_EQ(next_field(sv), "SET");
    EXPECT_EQ(next_field(sv), "");
    EXPECT_EQ(next_field(sv), "val");
}

TEST(UtilsTest, NextFieldLeavesRemainderForValue) {
    // Callers stop splitting after the fields they need, so a value can
    // legitimately contain further '|' characters.
    std::string_view sv = "SET|key|a|b|c";

    EXPECT_EQ(next_field(sv), "SET");
    EXPECT_EQ(next_field(sv), "key");
    EXPECT_EQ(sv, "a|b|c");
}

TEST(UtilsTest, ParseUint64Valid) {
    uint64_t out = 0;
    std::string err;

    EXPECT_TRUE(parse_uint64("12345", out, err));
    EXPECT_EQ(out, 12345u);
}

TEST(UtilsTest, ParseUint64RejectsNegative) {
    // user_id and similar fields are unsigned by contract, so a leading
    // '-' must be rejected with a specific reason rather than silently
    // parsed or wrapped.
    uint64_t out = 0;
    std::string err;

    EXPECT_FALSE(parse_uint64("-5", out, err));
    EXPECT_EQ(err, "NEGATIVE_NOT_ALLOWED");
}

TEST(UtilsTest, ParseUint64RejectsEmpty) {
    uint64_t out = 0;
    std::string err;

    EXPECT_FALSE(parse_uint64("", out, err));
    EXPECT_EQ(err, "EMPTY_FIELD");
}

TEST(UtilsTest, ParseUint64RejectsTrailingJunk) {
    // "123abc" must not silently parse as 123 -- any non-digit suffix
    // makes the whole field invalid.
    uint64_t out = 0;
    std::string err;

    EXPECT_FALSE(parse_uint64("123abc", out, err));
    EXPECT_EQ(err, "TRAILING_JUNK");
}