#pragma once
#include <string>
#include <vector>
#include <memory>
#include "lexer.h"

struct ASTNode {
    virtual ~ASTNode() = default;
};

struct IntLiteral : ASTNode { int value; IntLiteral(int v) : value(v) {} };
struct LongLongLiteral : ASTNode { long long value; LongLongLiteral(long long v) : value(v) {} };
struct DoubleLiteral : ASTNode { double value; DoubleLiteral(double v) : value(v) {} };
struct NullLiteral : ASTNode {};

struct StringLiteral : ASTNode {
    std::string value;
    bool isTemplate = false;
    StringLiteral(const std::string& v, bool templated = false) : value(v), isTemplate(templated) {}
};

struct BoolLiteral : ASTNode { bool value; BoolLiteral(bool v) : value(v) {} };
struct Identifier : ASTNode { std::string name; Identifier(const std::string& n) : name(n) {} };

struct UnaryExpression : ASTNode { std::string op; std::shared_ptr<ASTNode> value; };
struct BinaryExpression : ASTNode { std::string op; std::shared_ptr<ASTNode> left; std::shared_ptr<ASTNode> right; };
struct NullCoalesceExpression : ASTNode { std::shared_ptr<ASTNode> left; std::shared_ptr<ASTNode> right; };
struct TernaryExpression : ASTNode { std::shared_ptr<ASTNode> condition; std::shared_ptr<ASTNode> whenTrue; std::shared_ptr<ASTNode> whenFalse; };

struct NamedArgument { std::string name; std::shared_ptr<ASTNode> value; };
struct FunctionCall : ASTNode {
    std::string name;
    std::vector<std::shared_ptr<ASTNode>> args;
    std::vector<NamedArgument> namedArgs;
};

struct MethodCallExpression : ASTNode {
    std::shared_ptr<ASTNode> target;
    std::string method;
    std::vector<std::shared_ptr<ASTNode>> args;
    std::vector<NamedArgument> namedArgs;
};

struct NullSafeCallExpression : ASTNode {
    std::shared_ptr<ASTNode> target;
    std::string method;
    std::vector<std::shared_ptr<ASTNode>> args;
    std::vector<NamedArgument> namedArgs;
};

struct FieldAccessExpression : ASTNode { std::shared_ptr<ASTNode> target; std::string field; };
struct ArrayLiteral : ASTNode { std::vector<std::shared_ptr<ASTNode>> items; };
struct IndexExpression : ASTNode { std::shared_ptr<ASTNode> target; std::shared_ptr<ASTNode> index; };

struct VarDeclaration : ASTNode {
    std::string type;
    std::string name;
    std::shared_ptr<ASTNode> value;
    bool isConst = false;
};

struct Assignment : ASTNode { std::string name; std::string op = "="; std::shared_ptr<ASTNode> value; };
struct FieldAssignment : ASTNode { std::shared_ptr<ASTNode> target; std::string field; std::string op = "="; std::shared_ptr<ASTNode> value; };
struct IndexAssignment : ASTNode { std::string name; std::shared_ptr<ASTNode> index; std::shared_ptr<ASTNode> value; };

struct BlockStatement : ASTNode { std::vector<std::shared_ptr<ASTNode>> statements; };
struct IfStatement : ASTNode { std::shared_ptr<ASTNode> condition; std::shared_ptr<BlockStatement> thenBranch; std::shared_ptr<ASTNode> elseBranch; };
struct WhileStatement : ASTNode { std::shared_ptr<ASTNode> condition; std::shared_ptr<BlockStatement> body; };
struct ForRangeStatement : ASTNode { std::string variable; std::shared_ptr<ASTNode> start; std::shared_ptr<ASTNode> end; std::shared_ptr<ASTNode> step; std::shared_ptr<BlockStatement> body; };
struct BreakStatement : ASTNode {};
struct ContinueStatement : ASTNode {};
struct ReturnStatement : ASTNode { std::shared_ptr<ASTNode> value; };

struct FunctionDeclaration : ASTNode {
    std::string name;
    std::string returnType;
    std::vector<std::pair<std::string, std::string>> params;
    std::shared_ptr<BlockStatement> body;
};

struct TryCatchStatement : ASTNode { std::shared_ptr<BlockStatement> tryBlock; std::string errorName; std::shared_ptr<BlockStatement> catchBlock; };
struct IncludeStatement : ASTNode { std::string path; };

struct MatchPattern {
    std::string kind;
    std::string text;
    std::shared_ptr<ASTNode> value;
};
struct MatchArm { MatchPattern pattern; std::shared_ptr<ASTNode> guard; std::shared_ptr<ASTNode> body; };
struct MatchStatement : ASTNode { std::shared_ptr<ASTNode> subject; std::vector<MatchArm> arms; };

struct StructField { std::string type; std::string name; };
struct StructDeclaration : ASTNode { std::string name; std::vector<StructField> fields; };
struct StructLiteral : ASTNode { std::string typeName; std::vector<NamedArgument> fields; };

struct Program : ASTNode { std::vector<std::shared_ptr<ASTNode>> statements; };

class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::shared_ptr<Program> parse();

private:
    std::vector<Token> tokens;
    size_t pos;

    Token current();
    Token peek(size_t offset = 1);
    Token consume();
    Token expect(TokenType type);
    bool match(TokenType type);
    bool check(TokenType type);
    void consumeStatementEnd();

    std::shared_ptr<ASTNode> parseStatement();
    std::shared_ptr<ASTNode> parseVarDeclaration();
    std::shared_ptr<ASTNode> parseAssignment();
    std::shared_ptr<ASTNode> parseFieldAssignment();
    std::shared_ptr<BlockStatement> parseBlock();
    std::shared_ptr<ASTNode> parseIfStatement();
    std::shared_ptr<ASTNode> parseWhileStatement();
    std::shared_ptr<ASTNode> parseForStatement();
    std::shared_ptr<ASTNode> parseFunctionDeclaration();
    std::shared_ptr<ASTNode> parseReturnStatement();
    std::shared_ptr<ASTNode> parseTryCatchStatement();
    std::shared_ptr<ASTNode> parseIncludeStatement();
    std::shared_ptr<ASTNode> parseMatchStatement();
    std::shared_ptr<ASTNode> parseStructDeclaration();
    std::shared_ptr<ASTNode> parseFunctionCall(const std::string& name);
    void parseCallArguments(std::vector<std::shared_ptr<ASTNode>>& args, std::vector<NamedArgument>& namedArgs, const std::string& contextName);
    MatchPattern parseMatchPattern();
    std::shared_ptr<ASTNode> parseExpression();
    std::shared_ptr<ASTNode> parseTernary();
    std::shared_ptr<ASTNode> parseNullCoalesce();
    std::shared_ptr<ASTNode> parseLogicalOr();
    std::shared_ptr<ASTNode> parseLogicalAnd();
    std::shared_ptr<ASTNode> parseBitwiseOr();
    std::shared_ptr<ASTNode> parseBitwiseXor();
    std::shared_ptr<ASTNode> parseBitwiseAnd();
    std::shared_ptr<ASTNode> parseEquality();
    std::shared_ptr<ASTNode> parseComparison();
    std::shared_ptr<ASTNode> parseShift();
    std::shared_ptr<ASTNode> parseTerm();
    std::shared_ptr<ASTNode> parseFactor();
    std::shared_ptr<ASTNode> parseUnary();
    std::shared_ptr<ASTNode> parsePostfix();
    std::shared_ptr<ASTNode> parsePrimary();
    std::shared_ptr<ASTNode> parseArrayLiteral();
    std::shared_ptr<ASTNode> parseStructLiteral(const std::string& typeName);
    std::string parseTypeName();
    std::string parseQualifiedName();
    bool isDeclarationStart() const;
    bool isFieldAssignmentStart() const;
};
