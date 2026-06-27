#include "parser.h"
#include <stdexcept>
#include <sstream>

namespace {
std::runtime_error parseError(const Token& token, const std::string& message) {
    std::ostringstream oss;
    oss << "E_PARSE line=" << token.line << " column=" << token.column << " message=\"" << message << "\"";
    return std::runtime_error(oss.str());
}

bool isTypeToken(TokenType type) {
    return type == TYPE_INT || type == TYPE_LONG || type == TYPE_STRING || type == TYPE_BOOL;
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

    if (isTypeToken(t.type)) return parseVarDeclaration();
    if (t.type == FUNC) return parseFunctionDeclaration();
    if (t.type == RETURN) return parseReturnStatement();
    if (t.type == BREAK) { consume(); return std::make_shared<BreakStatement>(); }
    if (t.type == CONTINUE) { consume(); return std::make_shared<ContinueStatement>(); }
    if (t.type == IF) return parseIfStatement();
    if (t.type == WHILE) return parseWhileStatement();
    if (t.type == FOR) return parseForStatement();
    if (t.type == TRY) return parseTryCatchStatement();
    if (t.type == INCLUDE) return parseIncludeStatement();
    if (t.type == LBRACE) return parseBlock();

    if (t.type == IDENTIFIER && (peek().type == ASSIGN || peek().type == LBRACKET))
        return parseAssignment();

    return parseExpression();
}

std::string Parser::parseTypeName() {
    if (!isTypeToken(current().type)) throw parseError(current(), "Expected type name");
    std::string type = consume().value;
    if (match(LBRACKET)) {
        expect(RBRACKET);
        type += "[]";
    }
    return type;
}

std::shared_ptr<ASTNode> Parser::parseVarDeclaration() {
    auto decl = std::make_shared<VarDeclaration>();
    decl->type = parseTypeName();
    decl->name = expect(IDENTIFIER).value;
    expect(ASSIGN);
    decl->value = parseExpression();
    return decl;
}

std::shared_ptr<ASTNode> Parser::parseAssignment() {
    const std::string name = expect(IDENTIFIER).value;
    if (match(LBRACKET)) {
        auto assignment = std::make_shared<IndexAssignment>();
        assignment->name = name;
        assignment->index = parseExpression();
        expect(RBRACKET);
        expect(ASSIGN);
        assignment->value = parseExpression();
        return assignment;
    }

    auto assignment = std::make_shared<Assignment>();
    assignment->name = name;
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
        if (check(IF)) stmt->elseBranch = parseIfStatement();
        else stmt->elseBranch = parseBlock();
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

std::shared_ptr<ASTNode> Parser::parseForStatement() {
    auto stmt = std::make_shared<ForRangeStatement>();
    expect(FOR);
    stmt->variable = expect(IDENTIFIER).value;
    expect(IN);
    const std::string rangeName = expect(IDENTIFIER).value;
    if (rangeName != "range") throw parseError(current(), "Expected range(...) in for loop");
    expect(LPAREN);
    auto first = parseExpression();
    if (match(COMMA)) {
        stmt->start = first;
        stmt->end = parseExpression();
    }
    else {
        stmt->start = std::make_shared<IntLiteral>(0);
        stmt->end = first;
    }
    expect(RPAREN);
    stmt->body = parseBlock();
    return stmt;
}

std::shared_ptr<ASTNode> Parser::parseFunctionDeclaration() {
    auto fn = std::make_shared<FunctionDeclaration>();
    expect(FUNC);
    if (isTypeToken(current().type)) fn->returnType = parseTypeName();
    fn->name = expect(IDENTIFIER).value;
    expect(LPAREN);
    while (!check(RPAREN) && !check(END_OF_FILE)) {
        std::string type = parseTypeName();
        std::string name = expect(IDENTIFIER).value;
        fn->params.push_back({ type, name });
        if (match(COMMA)) continue;
        if (!check(RPAREN)) throw parseError(current(), "Expected , or ) in function parameters");
    }
    expect(RPAREN);
    if (match(ARROW)) fn->returnType = parseTypeName();
    if (fn->returnType.empty()) fn->returnType = "any";
    fn->body = parseBlock();
    return fn;
}

std::shared_ptr<ASTNode> Parser::parseReturnStatement() {
    auto stmt = std::make_shared<ReturnStatement>();
    expect(RETURN);
    if (!check(SEMICOLON) && !check(RBRACE) && !check(END_OF_FILE)) stmt->value = parseExpression();
    return stmt;
}

std::shared_ptr<ASTNode> Parser::parseTryCatchStatement() {
    auto stmt = std::make_shared<TryCatchStatement>();
    expect(TRY);
    stmt->tryBlock = parseBlock();
    expect(CATCH);
    if (check(IDENTIFIER)) stmt->errorName = consume().value;
    stmt->catchBlock = parseBlock();
    return stmt;
}

std::shared_ptr<ASTNode> Parser::parseIncludeStatement() {
    auto stmt = std::make_shared<IncludeStatement>();
    expect(INCLUDE);
    stmt->path = expect(STRING_LITERAL).value;
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

std::shared_ptr<ASTNode> Parser::parseExpression() { return parseLogicalOr(); }

std::shared_ptr<ASTNode> Parser::parseLogicalOr() {
    auto expr = parseLogicalAnd();
    while (check(LOGICAL_OR)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseLogicalAnd();
        expr = binary;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseLogicalAnd() {
    auto expr = parseBitwiseOr();
    while (check(LOGICAL_AND)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseBitwiseOr();
        expr = binary;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseBitwiseOr() {
    auto expr = parseBitwiseXor();
    while (check(BIT_OR)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseBitwiseXor();
        expr = binary;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseBitwiseXor() {
    auto expr = parseBitwiseAnd();
    while (check(BIT_XOR)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseBitwiseAnd();
        expr = binary;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseBitwiseAnd() {
    auto expr = parseEquality();
    while (check(BIT_AND)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseEquality();
        expr = binary;
    }
    return expr;
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
    auto expr = parseShift();
    while (check(LESS) || check(LESS_EQUALS) || check(GREATER) || check(GREATER_EQUALS)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseShift();
        expr = binary;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseShift() {
    auto expr = parseTerm();
    while (check(SHIFT_LEFT) || check(SHIFT_RIGHT)) {
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
    while (check(STAR) || check(SLASH) || check(PERCENT)) {
        auto binary = std::make_shared<BinaryExpression>();
        binary->op = consume().value;
        binary->left = expr;
        binary->right = parseUnary();
        expr = binary;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseUnary() {
    if (check(MINUS) || check(BANG) || check(BIT_NOT)) {
        auto unary = std::make_shared<UnaryExpression>();
        unary->op = consume().value;
        unary->value = parseUnary();
        return unary;
    }
    return parsePostfix();
}

std::shared_ptr<ASTNode> Parser::parsePostfix() {
    auto expr = parsePrimary();
    while (match(LBRACKET)) {
        auto index = std::make_shared<IndexExpression>();
        index->target = expr;
        index->index = parseExpression();
        expect(RBRACKET);
        expr = index;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseArrayLiteral() {
    auto arr = std::make_shared<ArrayLiteral>();
    expect(LBRACKET);
    while (!check(RBRACKET) && !check(END_OF_FILE)) {
        arr->items.push_back(parseExpression());
        if (match(COMMA)) continue;
        if (!check(RBRACKET)) throw parseError(current(), "Expected , or ] in array literal");
    }
    expect(RBRACKET);
    return arr;
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
    if (t.type == LBRACKET) return parseArrayLiteral();
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
