#pragma once
#pragma once
#include <string>
#include <vector>

enum TokenType {
    INT_LITERAL, STRING_LITERAL,
    TYPE_INT, TYPE_STRING, TYPE_BOOL,
    IF, ELSE, WHILE,
    DOT_CALL,
    EQUALS, NOT_EQUALS, ASSIGN,
    PLUS, MINUS,
    LPAREN, RPAREN, LBRACE, RBRACE,
    SEMICOLON, COMMA,
    IDENTIFIER,
    END_OF_FILE
};

struct Token {
    TokenType type;
    std::string value;
};

class Lexer {
public:
    Lexer(const std::string& source);
    std::vector<Token> tokenize();

private:
    std::string src;
    size_t pos;

    void skipWhitespace();
    Token readDotCall();
    Token readInt();
    Token readString();
    Token readIdentifierOrKeyword();
    Token readSymbol();
};