#include <catch2/catch_test_macros.hpp>

#include "css/tokenizer.hpp"

using namespace rendy::css;

TEST_CASE("tokenizes a simple rule", "[css][tokenizer]") {
    auto tokens = tokenize(".btn { color: #fff; }");
    REQUIRE(tokens[0].isDelim('.'));
    REQUIRE(tokens[1].is(TokenType::Ident));
    REQUIRE(tokens[1].value == "btn");
    REQUIRE(tokens[2].is(TokenType::Whitespace));
    REQUIRE(tokens[3].is(TokenType::LBrace));
    REQUIRE(tokens[5].is(TokenType::Ident));
    REQUIRE(tokens[5].value == "color");
    REQUIRE(tokens[6].is(TokenType::Colon));
    REQUIRE(tokens[8].is(TokenType::Hash));
    REQUIRE(tokens[8].value == "fff");
    REQUIRE(tokens[9].is(TokenType::Semicolon));
    REQUIRE(tokens.back().is(TokenType::End));
}

TEST_CASE("numbers, dimensions, percentages", "[css][tokenizer]") {
    auto tokens = tokenize("12px 1.5em 50% -3 .5");
    REQUIRE(tokens[0].is(TokenType::Dimension));
    REQUIRE(tokens[0].value == "12");
    REQUIRE(tokens[0].unit == "px");
    REQUIRE(tokens[2].is(TokenType::Dimension));
    REQUIRE(tokens[2].unit == "em");
    REQUIRE(tokens[4].is(TokenType::Percentage));
    REQUIRE(tokens[4].value == "50");
    REQUIRE(tokens[6].is(TokenType::Number));
    REQUIRE(tokens[6].value == "-3");
    REQUIRE(tokens[8].is(TokenType::Number));
    REQUIRE(tokens[8].value == ".5");
}

TEST_CASE("functions and strings", "[css][tokenizer]") {
    auto tokens = tokenize("rgb(1, 2, 3) \"Fira Sans\"");
    REQUIRE(tokens[0].is(TokenType::Function));
    REQUIRE(tokens[0].value == "rgb");
    REQUIRE(tokens[1].is(TokenType::Number));
    // rgb( 1 , _ 2 , _ 3 ) → RParen at index 8, then whitespace, then string.
    REQUIRE(tokens[8].is(TokenType::RParen));
    REQUIRE(tokens[10].is(TokenType::String));
    REQUIRE(tokens[10].value == "Fira Sans");
}

TEST_CASE("comments are skipped", "[css][tokenizer]") {
    auto tokens = tokenize("a /* comment { } */ b");
    REQUIRE(tokens[0].value == "a");
    REQUIRE(tokens[1].is(TokenType::Whitespace));
    REQUIRE(tokens[2].value == "b");
}

TEST_CASE("kebab-case identifiers stay whole", "[css][tokenizer]") {
    auto tokens = tokenize("flex-direction:row-reverse");
    REQUIRE(tokens[0].value == "flex-direction");
    REQUIRE(tokens[1].is(TokenType::Colon));
    REQUIRE(tokens[2].value == "row-reverse");
}
