#include "parser.h"
#include <stdexcept>
#include <sstream>

namespace {
std::runtime_error parseError(const Token& token, const std::string& message) {
    std::ostringstream oss;
    oss << "E_PARSE line=" << token.line << " column=" << token.column << " message=\"" << message << "\"";
    return std::runtime_error(oss.str());
}
}


Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens), pos(0) {}

Token Parser::current() {
    if (pos >= tokens.size()) return tokens.back();
    return tokens[pos];
}

Token Parser::peek(size_t offset) {
    const size_t index = pos + offset;
    if (index >= tokens.size()) return tokens.back();
    return tokens[index];
}

Token Parser::consume() { return tokens[pos++]; }

Token Parser::expect(TokenType type) {
    if (current().type != type)
        throw parseError(current(), "Unexpected token: " + current().value);
    return consume();
}

bool Parser::match(TokenType type) {
    if (!check(type)) return false;
    consume();
    return true;
}

bool Parser::check(TokenType type) {
    return current().type == type;
}

void Parser::consumeStatementEnd() {
    while (match(SEMICOLON)) {}
}

std::shared_ptr<Program> Parser::parse() {
    auto program = std::make_shared<Program>();
    while (!check(END_OF_FILE)) {
        consumeStatementEnd();
        if (check(END_OF_FILE)) break;
        program->statements.push_back(parseStatement());
        consumeStatementEnd();
    }
    return program;
}

std::shared_ptr<ASTNode> Parser::parseStatement() {
    Token t = current();

    if (t.type == TYPE_INT || t.type == TYPE_LONG || t.type == TYPE_STRING || t.type == TYPE_BOOL)
        return parseVarDeclaration();

    if (t.type == IF) return parseIfStatement();
    if (t.type == WHILE) return parseWhileStatement();
    if (t.type == LBRACE) return parseBlock();

    if (t.type == IDENTIFIER && peek().type == ASSIGN)
        return parseAssignment();

    return parseExpression();
}

std::shared_ptr<ASTNode> Parser::parseVarDeclaration() {
    auto decl = std::make_shared<VarDeclaration>();
    decl->type = consume().value;
    decl->name = expect(IDENTIFIER).value;
    expect(ASSIGN);
    decl->value = parseExpression();
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseAssignment() {
    auto assignment = std::make_shared<Assignment>();
    assignment->name = expect(IDENTIFIER).value;
    expect(ASSIGN);
    assignment->value = parseExpression();
    return assignment;
}

std::shared_ptr<BlockStatement> Parser::parseBlock() {
    auto block = std::make_shared<BlockStatement>();
    expect(LBRACE);

    while (!check(RBRACE) && !check(END_OF_FILE)) {
        consumeStatementEnd();
        if (check(RBRACE) || check(END_OF_FILE)) break;
        block->statements.push_back(parseStatement());
        consumeStatementEnd();
    }

    expect(RBRACE);
    return block;
}

std::shared_ptr<ASTNode> Parser::parseIfStatement() {
    auto stmt = std::make_shared<IfStatement>();
    expect(IF);
    stmt->condition = parseExpression();
    stmt->thenBranch = parseBlock();

    if (match(ELSE)) {
        if (check(IF)) {
            stmt->elseBranch = parseIfStatement();
        }
        else {
            stmt->elseBranch = parseBlock();
        }
    }

    return stmt;
}

std::shared_ptr<ASTNode> Parser::parseWhileStatement() {
    auto stmt = std::make_shared<WhileStatement>();
    expect(WHILE);
    stmt->condition = parseExpression();
    stmt->body = parseBlock();
    return stmt;
}

std::shared_ptr<ASTNode> Parser::parseFunctionCall(const std::string& name) {
    auto call = std::make_shared<FunctionCall>();
    call->name = name;
    expect(LPAREN);

    while (!check(RPAREN) && !check(END_OF_FILE)) {
        call->args.push_back(parseExpression());
        if (match(COMMA)) continue;
        if (!check(RPAREN)) throw parseError(current(), "Expected , or ) in call to " + name);
    }

    expect(RPAREN);
    return call;
}

std::shared_ptr<ASTNode> Parser::parseExpression() {
    return parseEquality();
}

std::shared_ptr<ASTNode> Parser::parseEquality() {
    auto expr = parseComparison();

    while (check(EQUALS) || check(NOT_EQUALS)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseComparison();
        expr = binary;
    }

    return expr;
}

std::shared_ptr<ASTNode> Parser::parseComparison() {
    auto expr = parseTerm();

    while (check(LESS) || check(LESS_EQUALS) || check(GREATER) || check(GREATER_EQUALS)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseTerm();
        expr = binary;
    }

    return expr;
}

std::shared_ptr<ASTNode> Parser::parseTerm() {
    auto expr = parseFactor();

    while (check(PLUS) || check(MINUS)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseFactor();
        expr = binary;
    }

    return expr;
}

std::shared_ptr<ASTNode> Parser::parseFactor() {
    auto expr = parseUnary();

    while (check(STAR) || check(SLASH)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseUnary();
        expr = binary;
    }

    return expr;
}

std::shared_ptr<ASTNode> Parser::parseUnary() {
    if (check(MINUS)) {
        auto unary = std::make_shared<UnaryExpression>();
        unary->op = consume().value;
        unary->value = parseUnary();
        return unary;
    }

    return parsePrimary();
}

std::shared_ptr<ASTNode> Parser::parsePrimary() {
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
    if (t.type == BOOL_LITERAL) {
        consume();
        return std::make_shared<BoolLiteral>(t.value == "true");
    }
    if (t.type == IDENTIFIER) {
        const std::string name = parseQualifiedName();
        if (check(LPAREN)) return parseFunctionCall(name);
        return std::make_shared<Identifier>(name);
    }
    if (match(LPAREN)) {
        auto expr = parseExpression();
        expect(RPAREN);
        return expr;
    }

    throw parseError(t, "Unknown expression: " + t.value);
}

std::string Parser::parseQualifiedName() {
    std::string name = expect(IDENTIFIER).value;

    while (match(DOT_CALL)) {
        name += ".";
        name += expect(IDENTIFIER).value;
    }

    return name;
}
