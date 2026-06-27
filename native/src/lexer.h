#pragma once
#include <string>
#include <vector>

enum TokenType {
    INT_LITERAL, HEX_LITERAL, STRING_LITERAL, BOOL_LITERAL,
    TYPE_INT, TYPE_LONG, TYPE_STRING, TYPE_BOOL,
    IF, ELSE, WHILE, FOR, IN, FUNC, RETURN, BREAK, CONTINUE, TRY, CATCH, INCLUDE,
    DOT_CALL,
    EQUALS, NOT_EQUALS, ASSIGN, ARROW,
    PLUS, MINUS, STAR, SLASH, PERCENT,
    LOGICAL_AND, LOGICAL_OR, BANG,
    BIT_AND, BIT_OR, BIT_XOR, BIT_NOT, SHIFT_LEFT, SHIFT_RIGHT,
    LESS, LESS_EQUALS, GREATER, GREATER_EQUALS,
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
    SEMICOLON, COMMA,
    IDENTIFIER,
    END_OF_FILE
};

struct Token {
    TokenType type;
    std::string value;
    int line = 1;
    int column = 1;
    size_t offset = 0;
};

class Lexer {
public:
    Lexer(const std::string& source);
    std::vector<Token> tokenize();

private:
    std::string src;
    size_t pos;
    int line;
    int column;

    void advance();
    void skipWhitespace();
    Token readHex();
    Token readInt();
    Token readString();
    Token readIdentifierOrKeyword();
    Token readSymbol();
};
