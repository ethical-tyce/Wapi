#include "lexer.h"
#include <cctype>
#include <stdexcept>

Lexer::Lexer(const std::string& source) : src(source), pos(0) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (pos < src.size()) {
        skipWhitespace();
        if (pos >= src.size()) break;

        char c = src[pos];

        if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
            while (pos < src.size() && src[pos] != '\n') pos++;
            continue;
        }

        if (c == '0' && pos + 1 < src.size() && src[pos + 1] == 'x')
            tokens.push_back(readHex());
        else if (c == '.') {
            tokens.push_back({ DOT_CALL, "." });
            pos++;
        }
        else if (isdigit(static_cast<unsigned char>(c))) tokens.push_back(readInt());
        else if (c == '"')          tokens.push_back(readString());
        else if (isalpha(static_cast<unsigned char>(c)) || c == '_') tokens.push_back(readIdentifierOrKeyword());
        else                        tokens.push_back(readSymbol());
    }
    tokens.push_back({ END_OF_FILE, "" });
    return tokens;
}

void Lexer::skipWhitespace() {
    while (pos < src.size() && isspace(static_cast<unsigned char>(src[pos]))) pos++;
}

Token Lexer::readHex() {
    pos += 2; // skip 0x
    std::string num;
    while (pos < src.size() && isxdigit(static_cast<unsigned char>(src[pos]))) num += src[pos++];
    return { HEX_LITERAL, num };
}

Token Lexer::readInt() {
    std::string num;
    while (pos < src.size() && isdigit(static_cast<unsigned char>(src[pos]))) num += src[pos++];
    return { INT_LITERAL, num };
}

Token Lexer::readString() {
    pos++;
    std::string str;
    while (pos < src.size() && src[pos] != '"') {
        if (src[pos] == '\\' && pos + 1 < src.size()) {
            char escaped = src[++pos];
            switch (escaped) {
            case 'n': str += '\n'; break;
            case 't': str += '\t'; break;
            case '"': str += '"'; break;
            case '\\': str += '\\'; break;
            default: str += escaped; break;
            }
            pos++;
            continue;
        }
        str += src[pos++];
    }
    if (pos >= src.size()) throw std::runtime_error("Unterminated string literal");
    pos++;
    return { STRING_LITERAL, str };
}

Token Lexer::readIdentifierOrKeyword() {
    std::string word;
    while (pos < src.size() && (isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) word += src[pos++];
    if (word == "if")     return { IF, word };
    if (word == "else")   return { ELSE, word };
    if (word == "while")  return { WHILE, word };
    if (word == "int")    return { TYPE_INT, word };
    if (word == "long")   return { TYPE_LONG, word };
    if (word == "string") return { TYPE_STRING, word };
    if (word == "bool")   return { TYPE_BOOL, word };
    if (word == "true")   return { BOOL_LITERAL, word };
    if (word == "false")  return { BOOL_LITERAL, word };
    return { IDENTIFIER, word };
}

Token Lexer::readSymbol() {
    char c = src[pos++];
    switch (c) {
    case '(': return { LPAREN, "(" };
    case ')': return { RPAREN, ")" };
    case '{': return { LBRACE, "{" };
    case '}': return { RBRACE, "}" };
    case ';': return { SEMICOLON, ";" };
    case ',': return { COMMA, "," };
    case '+': return { PLUS, "+" };
    case '-': return { MINUS, "-" };
    case '*': return { STAR, "*" };
    case '/': return { SLASH, "/" };
    case '=':
        if (pos < src.size() && src[pos] == '=') {
            pos++;
            return { EQUALS, "==" };
        }
        return { ASSIGN, "=" };
    case '!':
        if (pos < src.size() && src[pos] == '=') {
            pos++;
            return { NOT_EQUALS, "!=" };
        }
        break;
    case '<':
        if (pos < src.size() && src[pos] == '=') {
            pos++;
            return { LESS_EQUALS, "<=" };
        }
        return { LESS, "<" };
    case '>':
        if (pos < src.size() && src[pos] == '=') {
            pos++;
            return { GREATER_EQUALS, ">=" };
        }
        return { GREATER, ">" };
    }
    throw std::runtime_error("Unexpected character: " + std::string(1, c));
}
