#include "lexer.h"
#include <cctype>
#include <stdexcept>
#include <sstream>

namespace {
std::runtime_error lexError(int line, int column, const std::string& message) {
    std::ostringstream oss;
    oss << "E_LEX line=" << line << " column=" << column << " message=\"" << message << "\"";
    return std::runtime_error(oss.str());
}
}

Lexer::Lexer(const std::string& source) : src(source), pos(0), line(1), column(1) {}

std::vector<Token> Lexer::tokenize() {
    if (pos == 0 && src.size() >= 3 &&
        static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        pos = 3;
    }

    std::vector<Token> tokens;
    while (pos < src.size()) {
        skipWhitespace();
        if (pos >= src.size()) break;

        char c = src[pos];

        if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
            while (pos < src.size() && src[pos] != '\n') advance();
            continue;
        }
        if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*') {
            advance();
            advance();
            while (pos + 1 < src.size() && !(src[pos] == '*' && src[pos + 1] == '/')) advance();
            if (pos + 1 >= src.size()) throw lexError(line, column, "Unterminated block comment");
            advance();
            advance();
            continue;
        }

        if (c == '0' && pos + 1 < src.size() && src[pos + 1] == 'x') tokens.push_back(readHex());
        else if (c == '.') {
            const int tokenLine = line;
            const int tokenColumn = column;
            const size_t tokenOffset = pos;
            advance();
            if (pos < src.size() && src[pos] == '.' && pos + 1 < src.size() && src[pos + 1] == '.') {
                advance();
                advance();
                tokens.push_back({ DOT_CALL, "...", tokenLine, tokenColumn, tokenOffset });
            }
            else tokens.push_back({ DOT_CALL, ".", tokenLine, tokenColumn, tokenOffset });
        }
        else if (isdigit(static_cast<unsigned char>(c))) tokens.push_back(readInt());
        else if (c == '"') tokens.push_back(readString());
        else if (isalpha(static_cast<unsigned char>(c)) || c == '_') tokens.push_back(readIdentifierOrKeyword());
        else tokens.push_back(readSymbol());
    }
    tokens.push_back({ END_OF_FILE, "", line, column, pos });
    return tokens;
}

void Lexer::advance() {
    if (pos >= src.size()) return;
    if (src[pos] == '\n') {
        line++;
        column = 1;
    }
    else column++;
    pos++;
}

void Lexer::skipWhitespace() {
    while (pos < src.size() && isspace(static_cast<unsigned char>(src[pos]))) advance();
}

Token Lexer::readHex() {
    const int tokenLine = line;
    const int tokenColumn = column;
    const size_t tokenOffset = pos;
    advance();
    advance();
    std::string num;
    while (pos < src.size() && (isxdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) { if (src[pos] != '_') num += src[pos]; advance(); }
    if (num.empty()) throw lexError(tokenLine, tokenColumn, "Invalid hexadecimal literal");
    return { HEX_LITERAL, num, tokenLine, tokenColumn, tokenOffset };
}

Token Lexer::readInt() {
    const int tokenLine = line;
    const int tokenColumn = column;
    const size_t tokenOffset = pos;
    std::string num;
    while (pos < src.size() && (isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) { if (src[pos] != '_') num += src[pos]; advance(); }
    if (pos < src.size() && src[pos] == '.' && pos + 1 < src.size() && isdigit(static_cast<unsigned char>(src[pos + 1]))) {
        num += src[pos];
        advance();
        while (pos < src.size() && (isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) { if (src[pos] != '_') num += src[pos]; advance(); }
        return { DOUBLE_LITERAL, num, tokenLine, tokenColumn, tokenOffset };
    }
    return { INT_LITERAL, num, tokenLine, tokenColumn, tokenOffset };
}

Token Lexer::readString() {
    const int tokenLine = line;
    const int tokenColumn = column;
    const size_t tokenOffset = pos;
    if (pos + 2 < src.size() && src[pos + 1] == '"' && src[pos + 2] == '"') {
        advance();
        advance();
        advance();
        std::string str;
        while (pos + 2 < src.size() && !(src[pos] == '"' && src[pos + 1] == '"' && src[pos + 2] == '"')) {
            str += src[pos];
            advance();
        }
        if (pos + 2 >= src.size()) throw lexError(tokenLine, tokenColumn, "Unterminated multi-line string literal");
        advance();
        advance();
        advance();
        return { STRING_LITERAL, str, tokenLine, tokenColumn, tokenOffset, str.find('{') != std::string::npos };
    }

    advance();
    std::string str;
    while (pos < src.size() && src[pos] != '"') {
        if (src[pos] == '\\' && pos + 1 < src.size()) {
            advance();
            char escaped = src[pos];
            switch (escaped) {
            case 'n': str += '\n'; break;
            case 't': str += '\t'; break;
            case 'r': str += '\r'; break;
            case '"': str += '"'; break;
            case '\\': str += '\\'; break;
            default: str += escaped; break;
            }
            advance();
            continue;
        }
        str += src[pos];
        advance();
    }
    if (pos >= src.size()) throw lexError(tokenLine, tokenColumn, "Unterminated string literal");
    advance();
    return { STRING_LITERAL, str, tokenLine, tokenColumn, tokenOffset, str.find('{') != std::string::npos };
}

Token Lexer::readIdentifierOrKeyword() {
    const int tokenLine = line;
    const int tokenColumn = column;
    const size_t tokenOffset = pos;
    std::string word;
    while (pos < src.size() && (isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) { word += src[pos]; advance(); }
    if (word == "if")       return { IF, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "else")     return { ELSE, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "while")    return { WHILE, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "for")      return { FOR, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "in")       return { IN, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "func")     return { FUNC, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "return")   return { RETURN, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "break")    return { BREAK, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "continue") return { CONTINUE, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "try")      return { TRY, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "catch")    return { CATCH, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "include")  return { INCLUDE, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "match")    return { MATCH, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "struct")   return { STRUCT, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "var")      return { VAR, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "let")      return { LET, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "const")    return { CONST, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "int")      return { TYPE_INT, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "long")     return { TYPE_LONG, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "float")    return { TYPE_FLOAT, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "double")   return { TYPE_DOUBLE, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "string")   return { TYPE_STRING, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "bool")     return { TYPE_BOOL, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "true")     return { BOOL_LITERAL, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "false")    return { BOOL_LITERAL, word, tokenLine, tokenColumn, tokenOffset };
    if (word == "null")     return { NULL_LITERAL, word, tokenLine, tokenColumn, tokenOffset };
    return { IDENTIFIER, word, tokenLine, tokenColumn, tokenOffset };
}

Token Lexer::readSymbol() {
    const int tokenLine = line;
    const int tokenColumn = column;
    const size_t tokenOffset = pos;
    char c = src[pos];
    advance();
    switch (c) {
    case '(': return { LPAREN, "(", tokenLine, tokenColumn, tokenOffset };
    case ')': return { RPAREN, ")", tokenLine, tokenColumn, tokenOffset };
    case '{': return { LBRACE, "{", tokenLine, tokenColumn, tokenOffset };
    case '}': return { RBRACE, "}", tokenLine, tokenColumn, tokenOffset };
    case '[': return { LBRACKET, "[", tokenLine, tokenColumn, tokenOffset };
    case ']': return { RBRACKET, "]", tokenLine, tokenColumn, tokenOffset };
    case ';': return { SEMICOLON, ";", tokenLine, tokenColumn, tokenOffset };
    case ',': return { COMMA, ",", tokenLine, tokenColumn, tokenOffset };
    case '?':
        if (pos < src.size() && src[pos] == '.') { advance(); return { QUESTION_DOT, "?.", tokenLine, tokenColumn, tokenOffset }; }
        if (pos < src.size() && src[pos] == '?') { advance(); return { QUESTION_QUESTION, "??", tokenLine, tokenColumn, tokenOffset }; }
        return { QUESTION, "?", tokenLine, tokenColumn, tokenOffset };
    case ':': return { COLON, ":", tokenLine, tokenColumn, tokenOffset };
    case '+':
        if (pos < src.size() && src[pos] == '+') { advance(); return { INCREMENT, "++", tokenLine, tokenColumn, tokenOffset }; }
        if (pos < src.size() && src[pos] == '=') { advance(); return { PLUS_ASSIGN, "+=", tokenLine, tokenColumn, tokenOffset }; }
        return { PLUS, "+", tokenLine, tokenColumn, tokenOffset };
    case '-':
        if (pos < src.size() && src[pos] == '-') { advance(); return { DECREMENT, "--", tokenLine, tokenColumn, tokenOffset }; }
        if (pos < src.size() && src[pos] == '=') { advance(); return { MINUS_ASSIGN, "-=", tokenLine, tokenColumn, tokenOffset }; }
        if (pos < src.size() && src[pos] == '>') { advance(); return { ARROW, "->", tokenLine, tokenColumn, tokenOffset }; }
        return { MINUS, "-", tokenLine, tokenColumn, tokenOffset };
    case '*':
        if (pos < src.size() && src[pos] == '=') { advance(); return { STAR_ASSIGN, "*=", tokenLine, tokenColumn, tokenOffset }; }
        return { STAR, "*", tokenLine, tokenColumn, tokenOffset };
    case '/':
        if (pos < src.size() && src[pos] == '=') { advance(); return { SLASH_ASSIGN, "/=", tokenLine, tokenColumn, tokenOffset }; }
        return { SLASH, "/", tokenLine, tokenColumn, tokenOffset };
    case '%':
        if (pos < src.size() && src[pos] == '=') { advance(); return { PERCENT_ASSIGN, "%=", tokenLine, tokenColumn, tokenOffset }; }
        return { PERCENT, "%", tokenLine, tokenColumn, tokenOffset };
    case '^': return { BIT_XOR, "^", tokenLine, tokenColumn, tokenOffset };
    case '~': return { BIT_NOT, "~", tokenLine, tokenColumn, tokenOffset };
    case '&':
        if (pos < src.size() && src[pos] == '&') { advance(); return { LOGICAL_AND, "&&", tokenLine, tokenColumn, tokenOffset }; }
        return { BIT_AND, "&", tokenLine, tokenColumn, tokenOffset };
    case '|':
        if (pos < src.size() && src[pos] == '|') { advance(); return { LOGICAL_OR, "||", tokenLine, tokenColumn, tokenOffset }; }
        return { BIT_OR, "|", tokenLine, tokenColumn, tokenOffset };
    case '=':
        if (pos < src.size() && src[pos] == '=') { advance(); return { EQUALS, "==", tokenLine, tokenColumn, tokenOffset }; }
        if (pos < src.size() && src[pos] == '>') { advance(); return { FAT_ARROW, "=>", tokenLine, tokenColumn, tokenOffset }; }
        return { ASSIGN, "=", tokenLine, tokenColumn, tokenOffset };
    case '!':
        if (pos < src.size() && src[pos] == '=') { advance(); return { NOT_EQUALS, "!=", tokenLine, tokenColumn, tokenOffset }; }
        return { BANG, "!", tokenLine, tokenColumn, tokenOffset };
    case '<':
        if (pos < src.size() && src[pos] == '=') { advance(); return { LESS_EQUALS, "<=", tokenLine, tokenColumn, tokenOffset }; }
        if (pos < src.size() && src[pos] == '<') { advance(); return { SHIFT_LEFT, "<<", tokenLine, tokenColumn, tokenOffset }; }
        return { LESS, "<", tokenLine, tokenColumn, tokenOffset };
    case '>':
        if (pos < src.size() && src[pos] == '=') { advance(); return { GREATER_EQUALS, ">=", tokenLine, tokenColumn, tokenOffset }; }
        if (pos < src.size() && src[pos] == '>') { advance(); return { SHIFT_RIGHT, ">>", tokenLine, tokenColumn, tokenOffset }; }
        return { GREATER, ">", tokenLine, tokenColumn, tokenOffset };
    }
    throw lexError(tokenLine, tokenColumn, "Unexpected character: " + std::string(1, c));
}
