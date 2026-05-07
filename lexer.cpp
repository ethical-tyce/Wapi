#include "lexer.h"

Lexer::Lexer(const std::string& source) : src(source), pos(0) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (pos < src.size()) {
        skipWhitespace();
        if (pos >= src.size()) break;

        char c = src[pos];

        if (c == '0' && pos + 1 < src.size() && src[pos + 1] == 'x')
            tokens.push_back(readHex());
        else if (c == '.')          tokens.push_back({ DOT_CALL, "" });
        else if (isdigit(c))        tokens.push_back(readInt());
        else if (c == '"')          tokens.push_back(readString());
        else if (isalpha(c))        tokens.push_back(readIdentifierOrKeyword());
        else                        tokens.push_back(readSymbol());
    }
    tokens.push_back({ END_OF_FILE, "" });
    return tokens;
}

void Lexer::skipWhitespace() {
    while (pos < src.size() && isspace(src[pos])) pos++;
}

Token Lexer::readHex() {
    pos += 2; // skip 0x
    std::string num;
    while (pos < src.size() && isxdigit(src[pos])) num += src[pos++];
    return { HEX_LITERAL, num };
}

Token Lexer::readInt() {
    std::string num;
    while (pos < src.size() && isdigit(src[pos])) num += src[pos++];
    return { INT_LITERAL, num };
}

Token Lexer::readString() {
    pos++;
    std::string str;
    while (pos < src.size() && src[pos] != '"') str += src[pos++];
    pos++;
    return { STRING_LITERAL, str };
}

Token Lexer::readIdentifierOrKeyword() {
    std::string word;
    while (pos < src.size() && (isalnum(src[pos]) || src[pos] == '_')) word += src[pos++];
    if (word == "if")     return { IF, word };
    if (word == "else")   return { ELSE, word };
    if (word == "while")  return { WHILE, word };
    if (word == "int")    return { TYPE_INT, word };
    if (word == "long")   return { TYPE_LONG, word };
    if (word == "string") return { TYPE_STRING, word };
    if (word == "bool")   return { TYPE_BOOL, word };
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
    case '=': return { ASSIGN, "=" };
    }
    return { END_OF_FILE, "" };
}