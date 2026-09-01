#pragma once

// Small CSS tokenizer. Comments are skipped; whitespace is a token because
// it separates descendant combinators in selectors.

#include <string>
#include <string_view>
#include <vector>

namespace rendy::css {

enum class TokenType : uint8_t {
    Ident,      // flex-direction, div
    Hash,       // #main or #ff0000 (value is without '#')
    Number,     // 12, -1.5 (value holds the text)
    Dimension,  // 12px, 1.5em (value=number text, unit=suffix)
    Percentage, // 50%
    String,     // "Fira Sans" (value without quotes)
    Function,   // rgb(  (value without the paren)
    Whitespace,
    Colon, Semicolon, Comma, LBrace, RBrace, RParen,
    Delim, // any other single char: . > * etc (first char in value)
    End,
};

struct Token {
    TokenType type = TokenType::End;
    std::string value;
    std::string unit; // Dimension only

    [[nodiscard]] bool is(TokenType t) const { return type == t; }
    [[nodiscard]] bool isDelim(char c) const {
        return type == TokenType::Delim && !value.empty() && value[0] == c;
    }
};

/// Tokenizes the whole input. Never fails: unknown bytes become Delim tokens.
std::vector<Token> tokenize(std::string_view cssText);

} // namespace rendy::css
