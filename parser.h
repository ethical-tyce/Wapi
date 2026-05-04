#pragma once
#include <string>
#include <vector>
#include <memory>
#include "lexer.h"

// AST Nodes
struct ASTNode {
    virtual ~ASTNode() = default;
};

struct IntLiteral : ASTNode {
    int value;
    IntLiteral(int v) : value(v) {}
};

struct StringLiteral : ASTNode {
    std::string value;
    StringLiteral(const std::string& v) : value(v) {}
};

struct Identifier : ASTNode {
    std::string name;
    Identifier(const std::string& n) : name(n) {}
};

struct FunctionCall : ASTNode {
    std::string name;
    std::vector<std::shared_ptr<ASTNode>> args;
};

struct VarDeclaration : ASTNode {
    std::string type;
    std::string name;
    std::shared_ptr<ASTNode> value;
};

struct Program : ASTNode {
    std::vector<std::shared_ptr<ASTNode>> statements;
};

class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::shared_ptr<Program> parse();

private:
    std::vector<Token> tokens;
    size_t pos;

    Token current();
    Token consume();
    Token expect(TokenType type);

    std::shared_ptr<ASTNode> parseStatement();
    std::shared_ptr<ASTNode> parseVarDeclaration();
    std::shared_ptr<ASTNode> parseFunctionCall(const std::string& name);
    std::shared_ptr<ASTNode> parseExpression();
};