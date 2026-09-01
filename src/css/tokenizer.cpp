#include "css/tokenizer.hpp"

#include <cctype>

namespace rendy::css {
namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
           static_cast<unsigned char>(c) >= 0x80;
}
bool isIdentChar(char c) {
    return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c));
}
bool isNumberStart(std::string_view s, size_t i) {
    const char c = s[i];
    if (std::isdigit(static_cast<unsigned char>(c))) return true;
    if ((c == '-' || c == '+' || c == '.') && i + 1 < s.size())
        return std::isdigit(static_cast<unsigned char>(s[i + 1])) ||
               (c != '.' && s[i + 1] == '.' && i + 2 < s.size() &&
                std::isdigit(static_cast<unsigned char>(s[i + 2])));
    return false;
}

} // namespace

std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> tokens;
    size_t i = 0;
    const size_t n = s.size();

    auto readIdent = [&]() {
        const size_t start = i;
        while (i < n && isIdentChar(s[i])) i++;
        return std::string(s.substr(start, i - start));
    };

    while (i < n) {
        const char c = s[i];

        // Comments.
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = i + 2 <= n ? i + 2 : n;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) i++;
            if (!tokens.empty() && tokens.back().type != TokenType::Whitespace)
                tokens.push_back({TokenType::Whitespace, {}, {}});
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            i++;
            const size_t start = i;
            while (i < n && s[i] != quote) i++;
            tokens.push_back({TokenType::String, std::string(s.substr(start, i - start)), {}});
            if (i < n) i++; // closing quote
            continue;
        }
        if (c == '#') {
            i++;
            tokens.push_back({TokenType::Hash, readIdent(), {}});
            continue;
        }
        if (isNumberStart(s, i)) {
            const size_t start = i;
            if (s[i] == '-' || s[i] == '+') i++;
            while (i < n && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) i++;
            std::string number(s.substr(start, i - start));
            if (i < n && s[i] == '%') {
                i++;
                tokens.push_back({TokenType::Percentage, std::move(number), {}});
            } else if (i < n && isIdentStart(s[i])) {
                tokens.push_back({TokenType::Dimension, std::move(number), readIdent()});
            } else {
                tokens.push_back({TokenType::Number, std::move(number), {}});
            }
            continue;
        }
        if (isIdentStart(c) && !(c == '-' && i + 1 < n && !isIdentStart(s[i + 1]))) {
            std::string ident = readIdent();
            if (i < n && s[i] == '(') {
                i++;
                tokens.push_back({TokenType::Function, std::move(ident), {}});
            } else {
                tokens.push_back({TokenType::Ident, std::move(ident), {}});
            }
            continue;
        }

        i++;
        switch (c) {
        case ':': tokens.push_back({TokenType::Colon, {}, {}}); break;
        case ';': tokens.push_back({TokenType::Semicolon, {}, {}}); break;
        case ',': tokens.push_back({TokenType::Comma, {}, {}}); break;
        case '{': tokens.push_back({TokenType::LBrace, {}, {}}); break;
        case '}': tokens.push_back({TokenType::RBrace, {}, {}}); break;
        case ')': tokens.push_back({TokenType::RParen, {}, {}}); break;
        default: tokens.push_back({TokenType::Delim, std::string(1, c), {}}); break;
        }
    }
    tokens.push_back({TokenType::End, {}, {}});
    return tokens;
}

} // namespace rendy::css
