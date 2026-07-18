#include "parser.h"
#include <stdexcept>
#include <sstream>
#include <cctype>
#include <unordered_set>

namespace {
std::runtime_error parseError(const Token& token, const std::string& message) {
    std::ostringstream oss;
    oss << "E_PARSE line=" << token.line << " column=" << token.column << " message=\"" << message << "\"";
    return std::runtime_error(oss.str());
}

bool isTypeToken(TokenType type) {
    return type == TYPE_INT || type == TYPE_LONG || type == TYPE_STRING || type == TYPE_BOOL || type == TYPE_DOUBLE || type == TYPE_FLOAT;
}

bool isAssignmentOperator(TokenType type) {
    return type == ASSIGN || type == PLUS_ASSIGN || type == MINUS_ASSIGN || type == STAR_ASSIGN || type == SLASH_ASSIGN || type == PERCENT_ASSIGN;
}

bool isQualifiedRuntimePrefix(const std::string& name) {
    static const std::unordered_set<std::string> prefixes = {
        "proc", "mem", "thread", "window", "inject", "debug", "token", "handle", "runtime", "detect", "payload", "pe"
    };
    return prefixes.count(name) > 0;
}

bool isNameSegmentToken(TokenType type) {
    return type == IDENTIFIER || type == IF || type == ELSE || type == WHILE || type == FOR || type == IN ||
        type == FUNC || type == RETURN || type == BREAK || type == CONTINUE || type == TRY || type == CATCH ||
        type == INCLUDE || type == MATCH || type == STRUCT || type == VAR || type == LET || type == CONST;
}
}

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens), pos(0) {}

Token Parser::current() { return pos >= tokens.size() ? tokens.back() : tokens[pos]; }
Token Parser::peek(size_t offset) { const size_t index = pos + offset; return index >= tokens.size() ? tokens.back() : tokens[index]; }
Token Parser::consume() { return tokens[pos++]; }

Token Parser::expect(TokenType type) {
    if (current().type != type) throw parseError(current(), "Unexpected token: " + current().value);
    return consume();
}

Token Parser::expectNameSegment() {
    if (!isNameSegmentToken(current().type)) throw parseError(current(), "Unexpected token: " + current().value);
    return consume();
}

bool Parser::match(TokenType type) { if (!check(type)) return false; consume(); return true; }
bool Parser::check(TokenType type) { return current().type == type; }
void Parser::consumeStatementEnd() { while (match(SEMICOLON)) {} }

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

bool Parser::isDeclarationStart() const {
    const TokenType type = tokens[pos].type;
    if (type == VAR || type == LET || type == CONST || isTypeToken(type)) return true;
    return type == IDENTIFIER && pos + 2 < tokens.size() && tokens[pos + 1].type == IDENTIFIER && tokens[pos + 2].type == ASSIGN;
}

bool Parser::isFieldAssignmentStart() const {
    size_t look = pos;
    if (look >= tokens.size() || tokens[look].type != IDENTIFIER) return false;
    ++look;
    bool sawField = false;
    while (look + 1 < tokens.size() && tokens[look].type == DOT_CALL && isNameSegmentToken(tokens[look + 1].type)) {
        sawField = true;
        look += 2;
    }
    return sawField && look < tokens.size() && (isAssignmentOperator(tokens[look].type) || tokens[look].type == INCREMENT || tokens[look].type == DECREMENT);
}

std::shared_ptr<ASTNode> Parser::parseStatement() {
    Token t = current();
    if (isDeclarationStart()) return parseVarDeclaration();
    if (t.type == STRUCT) return parseStructDeclaration();
    if (t.type == FUNC) return parseFunctionDeclaration();
    if (t.type == RETURN) return parseReturnStatement();
    if (t.type == BREAK) { consume(); return std::make_shared<BreakStatement>(); }
    if (t.type == CONTINUE) { consume(); return std::make_shared<ContinueStatement>(); }
    if (t.type == IF) return parseIfStatement();
    if (t.type == WHILE) return parseWhileStatement();
    if (t.type == FOR) return parseForStatement();
    if (t.type == TRY) return parseTryCatchStatement();
    if (t.type == INCLUDE) return parseIncludeStatement();
    if (t.type == MATCH) return parseMatchStatement();
    if (t.type == LBRACE) return parseBlock();
    if ((t.type == INCREMENT || t.type == DECREMENT) && peek().type == IDENTIFIER) {
        Token op = consume();
        auto assignment = std::make_shared<Assignment>();
        assignment->name = expect(IDENTIFIER).value;
        assignment->op = op.type == INCREMENT ? "+=" : "-=";
        assignment->value = std::make_shared<IntLiteral>(1);
        return assignment;
    }
    if (isFieldAssignmentStart()) return parseFieldAssignment();
    if (t.type == IDENTIFIER && (isAssignmentOperator(peek().type) || peek().type == INCREMENT || peek().type == DECREMENT || peek().type == LBRACKET)) return parseAssignment();
    return parseExpression();
}

std::string Parser::parseTypeName() {
    std::string type;
    if (isTypeToken(current().type) || current().type == IDENTIFIER) type = consume().value;
    else throw parseError(current(), "Expected type name");
    if (match(LBRACKET)) { expect(RBRACKET); type += "[]"; }
    return type;
}

std::shared_ptr<ASTNode> Parser::parseVarDeclaration() {
    auto decl = std::make_shared<VarDeclaration>();
    if (match(VAR)) decl->type = "auto";
    else if (match(LET)) { decl->type = "auto"; decl->isConst = true; }
    else if (match(CONST)) {
        decl->isConst = true;
        decl->type = (isTypeToken(current().type) || (current().type == IDENTIFIER && peek().type == IDENTIFIER)) ? parseTypeName() : "auto";
    }
    else decl->type = parseTypeName();
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
    if (match(INCREMENT) || match(DECREMENT)) {
        assignment->op = tokens[pos - 1].type == INCREMENT ? "+=" : "-=";
        assignment->value = std::make_shared<IntLiteral>(1);
        return assignment;
    }
    if (isAssignmentOperator(current().type)) {
        assignment->op = consume().value;
        assignment->value = parseExpression();
        return assignment;
    }
    throw parseError(current(), "Expected assignment operator");
}

std::shared_ptr<ASTNode> Parser::parseFieldAssignment() {
    std::shared_ptr<ASTNode> target = std::make_shared<Identifier>(expect(IDENTIFIER).value);
    std::string field;
    while (match(DOT_CALL)) {
        field = expectNameSegment().value;
        if (peek().type == DOT_CALL) {
            auto access = std::make_shared<FieldAccessExpression>();
            access->target = target;
            access->field = field;
            target = access;
        }
    }
    auto assignment = std::make_shared<FieldAssignment>();
    assignment->target = target;
    assignment->field = field;
    if (match(INCREMENT) || match(DECREMENT)) {
        assignment->op = tokens[pos - 1].type == INCREMENT ? "+=" : "-=";
        assignment->value = std::make_shared<IntLiteral>(1);
        return assignment;
    }
    if (!isAssignmentOperator(current().type)) throw parseError(current(), "Expected field assignment operator");
    assignment->op = consume().value;
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
    if (match(ELSE)) stmt->elseBranch = check(IF) ? parseIfStatement() : parseBlock();
    return stmt;
}

std::shared_ptr<ASTNode> Parser::parseWhileStatement() { auto stmt = std::make_shared<WhileStatement>(); expect(WHILE); stmt->condition = parseExpression(); stmt->body = parseBlock(); return stmt; }

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
        if (match(COMMA)) stmt->step = parseExpression();
    } else {
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
    if (fn->returnType.empty()) fn->returnType = "auto";
    fn->body = parseBlock();
    return fn;
}

std::shared_ptr<ASTNode> Parser::parseReturnStatement() {
    auto stmt = std::make_shared<ReturnStatement>();
    expect(RETURN);
    if (!check(SEMICOLON) && !check(RBRACE) && !check(END_OF_FILE)) stmt->value = parseExpression();
    return stmt;
}

std::shared_ptr<ASTNode> Parser::parseTryCatchStatement() { auto stmt = std::make_shared<TryCatchStatement>(); expect(TRY); stmt->tryBlock = parseBlock(); expect(CATCH); if (check(IDENTIFIER)) stmt->errorName = consume().value; stmt->catchBlock = parseBlock(); return stmt; }
std::shared_ptr<ASTNode> Parser::parseIncludeStatement() { auto stmt = std::make_shared<IncludeStatement>(); expect(INCLUDE); stmt->path = expect(STRING_LITERAL).value; return stmt; }

std::shared_ptr<ASTNode> Parser::parseStructDeclaration() {
    auto decl = std::make_shared<StructDeclaration>();
    expect(STRUCT);
    decl->name = expect(IDENTIFIER).value;
    expect(LBRACE);
    while (!check(RBRACE) && !check(END_OF_FILE)) {
        consumeStatementEnd();
        if (check(RBRACE)) break;
        StructField field{ parseTypeName(), expect(IDENTIFIER).value };
        decl->fields.push_back(field);
        consumeStatementEnd();
        match(COMMA);
    }
    expect(RBRACE);
    return decl;
}

MatchPattern Parser::parseMatchPattern() {
    MatchPattern pattern;
    if (check(IDENTIFIER) && current().value == "_") { consume(); pattern.kind = "default"; pattern.text = "_"; return pattern; }
    if (check(IDENTIFIER)) { pattern.kind = "binding"; pattern.text = consume().value; return pattern; }
    if (check(NULL_LITERAL)) { pattern.kind = "literal"; pattern.value = std::make_shared<NullLiteral>(); consume(); return pattern; }
    if (check(BOOL_LITERAL)) { pattern.kind = "literal"; pattern.value = std::make_shared<BoolLiteral>(current().value == "true"); consume(); return pattern; }
    if (check(INT_LITERAL)) { pattern.kind = "literal"; pattern.value = std::make_shared<IntLiteral>(std::stoi(consume().value)); return pattern; }
    if (check(HEX_LITERAL)) { pattern.kind = "literal"; pattern.value = std::make_shared<LongLongLiteral>(std::stoull(consume().value, nullptr, 16)); return pattern; }
    if (check(DOUBLE_LITERAL)) { pattern.kind = "literal"; pattern.value = std::make_shared<DoubleLiteral>(std::stod(consume().value)); return pattern; }
    if (check(STRING_LITERAL)) { Token t = consume(); pattern.kind = "literal"; pattern.value = std::make_shared<StringLiteral>(t.value, t.isTemplate); return pattern; }
    throw parseError(current(), "Expected match pattern");
}

std::shared_ptr<ASTNode> Parser::parseMatchStatement() {
    auto stmt = std::make_shared<MatchStatement>();
    expect(MATCH);
    if (check(IDENTIFIER) && peek().type == LBRACE) {
        stmt->subject = std::make_shared<Identifier>(consume().value);
    } else {
        stmt->subject = parseExpression();
    }
    expect(LBRACE);
    while (!check(RBRACE) && !check(END_OF_FILE)) {
        consumeStatementEnd();
        if (check(RBRACE)) break;
        MatchArm arm;
        arm.pattern = parseMatchPattern();
        if (match(IF)) arm.guard = parseExpression();
        expect(FAT_ARROW);
        arm.body = check(LBRACE) ? std::static_pointer_cast<ASTNode>(parseBlock()) : parseStatement();
        stmt->arms.push_back(arm);
        consumeStatementEnd();
        match(COMMA);
    }
    expect(RBRACE);
    return stmt;
}

void Parser::parseCallArguments(std::vector<std::shared_ptr<ASTNode>>& args, std::vector<NamedArgument>& namedArgs, const std::string& contextName) {
    expect(LPAREN);
    while (!check(RPAREN) && !check(END_OF_FILE)) {
        if (current().type == IDENTIFIER && peek().type == COLON) {
            NamedArgument named;
            named.name = consume().value;
            expect(COLON);
            named.value = parseExpression();
            namedArgs.push_back(named);
            args.push_back(named.value);
        } else {
            args.push_back(parseExpression());
        }
        if (match(COMMA)) continue;
        if (!check(RPAREN)) throw parseError(current(), "Expected , or ) in call to " + contextName);
    }
    expect(RPAREN);
}

std::shared_ptr<ASTNode> Parser::parseFunctionCall(const std::string& name) {
    auto call = std::make_shared<FunctionCall>();
    call->name = name;
    parseCallArguments(call->args, call->namedArgs, name);
    return call;
}

std::shared_ptr<ASTNode> Parser::parseExpression() { return parseTernary(); }
std::shared_ptr<ASTNode> Parser::parseTernary() {
    auto expr = parseNullCoalesce();
    if (match(QUESTION)) {
        auto ternary = std::make_shared<TernaryExpression>();
        ternary->condition = expr;
        ternary->whenTrue = parseExpression();
        expect(COLON);
        ternary->whenFalse = parseExpression();
        return ternary;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseNullCoalesce() {
    auto expr = parseLogicalOr();
    while (match(QUESTION_QUESTION)) {
        auto coalesce = std::make_shared<NullCoalesceExpression>();
        coalesce->left = expr;
        coalesce->right = parseLogicalOr();
        expr = coalesce;
    }
    return expr;
}

std::shared_ptr<ASTNode> Parser::parseLogicalOr() { auto expr = parseLogicalAnd(); while (check(LOGICAL_OR)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseLogicalAnd(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseLogicalAnd() { auto expr = parseBitwiseOr(); while (check(LOGICAL_AND)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseBitwiseOr(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseBitwiseOr() { auto expr = parseBitwiseXor(); while (check(BIT_OR)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseBitwiseXor(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseBitwiseXor() { auto expr = parseBitwiseAnd(); while (check(BIT_XOR)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseBitwiseAnd(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseBitwiseAnd() { auto expr = parseEquality(); while (check(BIT_AND)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseEquality(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseEquality() { auto expr = parseComparison(); while (check(EQUALS) || check(NOT_EQUALS)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseComparison(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseComparison() { auto expr = parseShift(); while (check(LESS) || check(LESS_EQUALS) || check(GREATER) || check(GREATER_EQUALS)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseShift(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseShift() { auto expr = parseTerm(); while (check(SHIFT_LEFT) || check(SHIFT_RIGHT)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseTerm(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseTerm() { auto expr = parseFactor(); while (check(PLUS) || check(MINUS)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseFactor(); expr = binary; } return expr; }
std::shared_ptr<ASTNode> Parser::parseFactor() { auto expr = parseUnary(); while (check(STAR) || check(SLASH) || check(PERCENT)) { auto binary = std::make_shared<BinaryExpression>(); binary->op = consume().value; binary->left = expr; binary->right = parseUnary(); expr = binary; } return expr; }

std::shared_ptr<ASTNode> Parser::parseUnary() {
    if (check(MINUS) || check(BANG) || check(BIT_NOT)) {
        auto unary = std::make_shared<UnaryExpression>();
        unary->op = consume().value;
        unary->value = parseUnary();
        return unary;
    }
    if ((check(INCREMENT) || check(DECREMENT)) && peek().type == IDENTIFIER) {
        Token op = consume();
        auto assignment = std::make_shared<Assignment>();
        assignment->name = expect(IDENTIFIER).value;
        assignment->op = op.type == INCREMENT ? "+=" : "-=";
        assignment->value = std::make_shared<IntLiteral>(1);
        return assignment;
    }
    return parsePostfix();
}

std::shared_ptr<ASTNode> Parser::parsePostfix() {
    auto expr = parsePrimary();
    while (true) {
        if (match(LBRACKET)) {
            auto index = std::make_shared<IndexExpression>();
            index->target = expr;
            index->index = parseExpression();
            expect(RBRACKET);
            expr = index;
            continue;
        }
        if (match(DOT_CALL) || match(QUESTION_DOT)) {
            const bool nullSafe = tokens[pos - 1].type == QUESTION_DOT;
            const std::string member = expectNameSegment().value;
            if (check(LPAREN)) {
                if (nullSafe) {
                    auto call = std::make_shared<NullSafeCallExpression>();
                    call->target = expr;
                    call->method = member;
                    parseCallArguments(call->args, call->namedArgs, member);
                    expr = call;
                } else {
                    auto call = std::make_shared<MethodCallExpression>();
                    call->target = expr;
                    call->method = member;
                    parseCallArguments(call->args, call->namedArgs, member);
                    expr = call;
                }
            } else {
                if (nullSafe) throw parseError(current(), "?. currently requires a method call");
                auto field = std::make_shared<FieldAccessExpression>();
                field->target = expr;
                field->field = member;
                expr = field;
            }
            continue;
        }
        break;
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

std::shared_ptr<ASTNode> Parser::parseStructLiteral(const std::string& typeName) {
    auto literal = std::make_shared<StructLiteral>();
    literal->typeName = typeName;
    expect(LBRACE);
    while (!check(RBRACE) && !check(END_OF_FILE)) {
        NamedArgument field;
        field.name = expect(IDENTIFIER).value;
        expect(COLON);
        field.value = parseExpression();
        literal->fields.push_back(field);
        if (match(COMMA)) continue;
        if (!check(RBRACE)) throw parseError(current(), "Expected , or } in struct literal");
    }
    expect(RBRACE);
    return literal;
}

std::shared_ptr<ASTNode> Parser::parsePrimary() {
    Token t = current();
    if (t.type == HEX_LITERAL) { consume(); return std::make_shared<LongLongLiteral>(std::stoull(t.value, nullptr, 16)); }
    if (t.type == INT_LITERAL) { consume(); return std::make_shared<IntLiteral>(std::stoi(t.value)); }
    if (t.type == DOUBLE_LITERAL) { consume(); return std::make_shared<DoubleLiteral>(std::stod(t.value)); }
    if (t.type == NULL_LITERAL) { consume(); return std::make_shared<NullLiteral>(); }
    if (t.type == STRING_LITERAL) { consume(); return std::make_shared<StringLiteral>(t.value, t.isTemplate); }
    if (t.type == BOOL_LITERAL) { consume(); return std::make_shared<BoolLiteral>(t.value == "true"); }
    if (t.type == LBRACKET) return parseArrayLiteral();
    if (t.type == IDENTIFIER) {
        const std::string first = consume().value;
        if (isQualifiedRuntimePrefix(first) && check(DOT_CALL)) {
            --pos;
            const std::string name = parseQualifiedName();
            if (check(LPAREN)) return parseFunctionCall(name);
            return std::make_shared<Identifier>(name);
        }
        if (check(LBRACE) && !first.empty() && std::isupper(static_cast<unsigned char>(first[0]))) return parseStructLiteral(first);
        if (check(LPAREN)) return parseFunctionCall(first);
        return std::make_shared<Identifier>(first);
    }
    if (match(LPAREN)) { auto expr = parseExpression(); expect(RPAREN); return expr; }
    throw parseError(t, "Unknown expression: " + t.value);
}

std::string Parser::parseQualifiedName() {
    std::string name = expect(IDENTIFIER).value;
    while (match(DOT_CALL)) { name += "."; name += expectNameSegment().value; }
    return name;
}
