#include "parser.h"
#include <stdexcept>

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens), pos(0) {}

Token Parser::current() { return tokens[pos]; }
Token Parser::consume() { return tokens[pos++]; }

Token Parser::expect(TokenType type) {
    if (current().type != type)
        throw std::runtime_error("Unexpected token: " + current().value);
    return consume();
}

std::shared_ptr<Program> Parser::parse() {
    auto program = std::make_shared<Program>();
    while (current().type != END_OF_FILE) {
        program->statements.push_back(parseStatement());
    }
    return program;
}

std::shared_ptr<ASTNode> Parser::parseStatement() {
    Token t = current();

    if (t.type == TYPE_INT || t.type == TYPE_LONG || t.type == TYPE_STRING || t.type == TYPE_BOOL)
        return parseVarDeclaration();

    if (t.type == DOT_CALL) {
        consume();
        return parseFunctionCall(t.value);
    }

    if (t.type == IDENTIFIER) {
        consume();
        if (current().type == LPAREN)
            return parseFunctionCall(t.value);
    }

    throw std::runtime_error("Unknown statement: " + t.value);
}

std::shared_ptr<ASTNode> Parser::parseVarDeclaration() {
    auto decl = std::make_shared<VarDeclaration>();
    decl->type = consume().value;
    decl->name = expect(IDENTIFIER).value;
    expect(ASSIGN);
    decl->value = parseExpression();
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseFunctionCall(const std::string& name) {
    auto call = std::make_shared<FunctionCall>();
    call->name = name;
    expect(LPAREN);
    while (current().type != RPAREN) {
        call->args.push_back(parseExpression());
        if (current().type == COMMA) consume();
    }
    expect(RPAREN);
    return call;
}

std::shared_ptr<ASTNode> Parser::parseExpression() {
    Token t = current();

    if (t.type == HEX_LITERAL) {
        consume();
        return std::make_shared<LongLongLiteral>(std::stoull(t.value, nullptr, 16));
    }
    if (t.type == INT_LITERAL) {
        consume();
        return std::make_shared<IntLiteral>(std::stoi(t.value));
    }
    if (t.type == STRING_LITERAL) {
        consume();
        return std::make_shared<StringLiteral>(t.value);
    }
    if (t.type == IDENTIFIER) {
        consume();
        if (current().type == LPAREN)
            return parseFunctionCall(t.value);
        return std::make_shared<Identifier>(t.value);
    }
    if (t.type == DOT_CALL) {
        consume();
        return parseFunctionCall(t.value);
    }

    throw std::runtime_error("Unknown expression: " + t.value);
}