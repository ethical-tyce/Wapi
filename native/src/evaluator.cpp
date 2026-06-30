#include "evaluator.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <optional>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
namespace {
bool isNumericValue(const WapiValue& value) {
    return std::holds_alternative<int>(value) || std::holds_alternative<long long>(value) || std::holds_alternative<double>(value);
}
long long numericValue(const WapiValue& value) {
    if (auto p = std::get_if<int>(&value)) return *p;
    if (auto p = std::get_if<long long>(&value)) return *p;
    return static_cast<long long>(std::get<double>(value));
}
double numericDoubleValue(const WapiValue& value) {
    if (auto p = std::get_if<int>(&value)) return static_cast<double>(*p);
    if (auto p = std::get_if<long long>(&value)) return static_cast<double>(*p);
    return std::get<double>(value);
}
std::string jsonEscape(const std::string& value) {
    std::ostringstream oss;
    for (char ch : value) {
        switch (ch) {
        case '\\': oss << "\\\\"; break;
        case '"': oss << "\\\""; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default: oss << ch; break;
        }
    }
    return oss.str();
}
bool valuesEqual(const WapiValue& left, const WapiValue& right) {
    if (isNumericValue(left) && isNumericValue(right)) {
        return numericDoubleValue(left) == numericDoubleValue(right);
    }
    if (auto l = std::get_if<std::string>(&left)) {
        if (auto r = std::get_if<std::string>(&right)) return *l == *r;
    }
    if (auto l = std::get_if<bool>(&left)) {
        if (auto r = std::get_if<bool>(&right)) return *l == *r;
    }
    if (auto l = std::get_if<WapiArrayPtr>(&left)) {
        if (auto r = std::get_if<WapiArrayPtr>(&right)) return l->get() == r->get();
    }
    if (auto l = std::get_if<WapiStructPtr>(&left)) {
        if (auto r = std::get_if<WapiStructPtr>(&right)) return l->get() == r->get();
    }
    return false;
}
struct BreakSignal {};
struct ContinueSignal {};
struct ReturnSignal { WapiValue value; };
std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}
std::string trimAscii(std::string value) {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}
std::string valueTypeName(const WapiValue& value) {
    if (std::holds_alternative<std::monostate>(value)) return "null";
    if (std::holds_alternative<int>(value)) return "int";
    if (std::holds_alternative<long long>(value)) return "long";
    if (std::holds_alternative<double>(value)) return "double";
    if (std::holds_alternative<std::string>(value)) return "string";
    if (std::holds_alternative<bool>(value)) return "bool";
    if (std::holds_alternative<WapiArrayPtr>(value)) return "array";
    if (auto p = std::get_if<WapiStructPtr>(&value)) return *p ? (*p)->typeName : "struct";
    return "unknown";
}
std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (length <= 0) return "";
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        result.data(),
        length,
        nullptr,
        nullptr
    );
    return result;
}
} // namespace
Evaluator::Evaluator(const WapiRuntimeOptions& options) : options(options) {
    if (options.timeoutMs > 0) {
        timeoutEnabled = true;
        timeoutDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
    }
}
Evaluator::~Evaluator() {
    cleanupTrackedResources();
}
void Evaluator::enforceTimeout() const {
    if (timeoutEnabled && std::chrono::steady_clock::now() >= timeoutDeadline) {
        throw std::runtime_error("E_TIMEOUT: exceeded " + std::to_string(options.timeoutMs) + "ms");
    }
}
std::string Evaluator::valueToString(const WapiValue& value) const {
    if (std::holds_alternative<std::monostate>(value)) return "null";
    if (auto p = std::get_if<int>(&value)) return std::to_string(*p);
    if (auto p = std::get_if<long long>(&value)) return std::to_string(*p);
    if (auto p = std::get_if<double>(&value)) { std::ostringstream oss; oss << *p; return oss.str(); }
    if (auto p = std::get_if<std::string>(&value)) return *p;
    if (auto p = std::get_if<bool>(&value)) return *p ? "true" : "false";
    if (auto p = std::get_if<WapiArrayPtr>(&value)) {
        std::ostringstream oss;
        oss << "[";
        if (*p) {
            for (size_t i = 0; i < (*p)->values.size(); ++i) {
                if (i) oss << ", ";
                oss << valueToString((*p)->values[i]);
            }
        }
        oss << "]";
        return oss.str();
    }
    if (auto p = std::get_if<WapiStructPtr>(&value)) {
        if (!*p) return "null";
        std::ostringstream oss;
        oss << (*p)->typeName << "{";
        bool first = true;
        for (const auto& [name, field] : (*p)->fields) {
            if (!first) oss << ", ";
            oss << name << ": " << valueToString(field);
            first = false;
        }
        oss << "}";
        return oss.str();
    }
    return "";
}
bool Evaluator::typeMatches(const std::string& typeName, const WapiValue& value) const {
    if (typeName == "any" || typeName == "auto") return true;
    if (typeName == "int") return std::holds_alternative<int>(value);
    if (typeName == "long") return isNumericValue(value);
    if (typeName == "float" || typeName == "double") return isNumericValue(value);
    if (typeName == "string") return std::holds_alternative<std::string>(value);
    if (typeName == "bool") return std::holds_alternative<bool>(value);
    if (std::holds_alternative<std::monostate>(value)) return true;
    if (typeName.size() > 2 && typeName.substr(typeName.size() - 2) == "[]") return std::holds_alternative<WapiArrayPtr>(value);
    if (auto p = std::get_if<WapiStructPtr>(&value)) return *p && (*p)->typeName == typeName;
    return true;
}
WapiArrayPtr Evaluator::asArrayValue(const WapiValue& value, const std::string& context) const {
    if (auto p = std::get_if<WapiArrayPtr>(&value)) {
        if (*p) return *p;
    }
    throw std::runtime_error("E_TYPE:" + context + " expected array");
}
bool Evaluator::isTruthy(const WapiValue& value) const {
    if (std::holds_alternative<std::monostate>(value)) return false;
    if (auto p = std::get_if<int>(&value)) return *p != 0;
    if (auto p = std::get_if<long long>(&value)) return *p != 0;
    if (auto p = std::get_if<double>(&value)) return *p != 0.0;
    if (auto p = std::get_if<std::string>(&value)) return !p->empty();
    if (auto p = std::get_if<bool>(&value)) return *p;
    if (auto p = std::get_if<WapiArrayPtr>(&value)) return *p && !(*p)->values.empty();
    if (auto p = std::get_if<WapiStructPtr>(&value)) return static_cast<bool>(*p);
    return false;
}
long long Evaluator::asNumberValue(const WapiValue& value, const std::string& context) const {
    if (auto p = std::get_if<int>(&value)) return *p;
    if (auto p = std::get_if<long long>(&value)) return *p;
    if (auto p = std::get_if<double>(&value)) return static_cast<long long>(*p);
    throw std::runtime_error("E_TYPE:" + context + " expected number");
}
void Evaluator::run(std::shared_ptr<Program> program) {
    try {
        for (auto& stmt : program->statements) {
            enforceTimeout();
            evalNode(stmt);
        }
        if (options.jsonOutput) {
            std::ostringstream payload;
            payload << "{";
            bool first = true;
            for (const auto& [name, value] : variables) {
                if (!first) payload << ",";
                payload << "\"" << jsonEscape(name) << "\":\"" << jsonEscape(valueToString(value)) << "\"";
                first = false;
            }
            payload << "}";
            emitJsonEvent("variables", payload.str());
        }
    }
    catch (const BreakSignal&) {
        throw std::runtime_error("E_BREAK_OUTSIDE_LOOP");
    }
    catch (const ContinueSignal&) {
        throw std::runtime_error("E_CONTINUE_OUTSIDE_LOOP");
    }
    catch (const ReturnSignal&) {
        throw std::runtime_error("E_RETURN_OUTSIDE_FUNCTION");
    }
}
WapiValue Evaluator::evalNode(std::shared_ptr<ASTNode> node) {
    enforceTimeout();
    if (auto n = std::dynamic_pointer_cast<IntLiteral>(node)) return n->value;
    if (auto n = std::dynamic_pointer_cast<LongLongLiteral>(node)) return n->value;
    if (auto n = std::dynamic_pointer_cast<DoubleLiteral>(node)) return n->value;
    if (std::dynamic_pointer_cast<NullLiteral>(node)) return std::monostate{};
    if (auto n = std::dynamic_pointer_cast<StringLiteral>(node)) {
        std::string out = n->value;
        size_t start = 0;
        while ((start = out.find("${", start)) != std::string::npos) {
            const size_t end = out.find("}", start + 2);
            if (end == std::string::npos) break;
            const std::string name = out.substr(start + 2, end - start - 2);
            auto found = variables.find(name);
            if (found != variables.end()) {
                const std::string replacement = valueToString(found->second);
                out.replace(start, end - start + 1, replacement);
                start += replacement.size();
            } else {
                start = end + 1;
            }
        }
        if (n->isTemplate) {
            start = 0;
            while ((start = out.find("{", start)) != std::string::npos) {
                if (start > 0 && out[start - 1] == '$') { ++start; continue; }
                const size_t end = out.find("}", start + 1);
                if (end == std::string::npos) break;
                const std::string expression = out.substr(start + 1, end - start - 1);
                try {
                    Lexer lexer(expression);
                    Parser parser(lexer.tokenize());
                    auto program = parser.parse();
                    if (!program->statements.empty()) {
                        const std::string replacement = valueToString(evalNode(program->statements.front()));
                        out.replace(start, end - start + 1, replacement);
                        start += replacement.size();
                        continue;
                    }
                } catch (...) {}
                start = end + 1;
            }
        }
        return out;
    }
    if (auto n = std::dynamic_pointer_cast<BoolLiteral>(node)) return n->value;
    if (auto n = std::dynamic_pointer_cast<ArrayLiteral>(node)) {
        auto array = std::make_shared<WapiArray>();
        for (auto& item : n->items) array->values.push_back(evalNode(item));
        return array;
    }
    if (auto n = std::dynamic_pointer_cast<Identifier>(node)) {
        if (variables.count(n->name)) return variables[n->name];
        throw std::runtime_error("E_UNDEFINED_VAR: " + n->name);
    }
    if (auto n = std::dynamic_pointer_cast<VarDeclaration>(node)) {
        WapiValue val = evalNode(n->value);
        if (!typeMatches(n->type, val)) throw std::runtime_error("E_TYPE:" + n->name + " expected " + n->type);
        variables[n->name] = val;
        if (n->isConst) immutableBindings.insert(n->name);
        return val;
    }
    if (auto n = std::dynamic_pointer_cast<Assignment>(node)) {
        if (immutableBindings.count(n->name)) throw std::runtime_error("E_CONST_ASSIGN:" + n->name);
        if (!variables.count(n->name)) throw std::runtime_error("E_UNDEFINED_VAR: " + n->name);
        WapiValue val = evalNode(n->value);
        if (n->op != "=") {
            WapiValue current = variables[n->name];
            BinaryExpression binary;
            binary.left = std::make_shared<Identifier>(n->name);
            binary.right = n->value;
            binary.op = n->op.substr(0, 1);
            val = evalBinaryExpression(binary);
        }
        variables[n->name] = val;
        return val;
    }
    if (auto n = std::dynamic_pointer_cast<IndexAssignment>(node)) {
        if (!variables.count(n->name)) throw std::runtime_error("E_UNDEFINED_VAR: " + n->name);
        auto array = asArrayValue(variables[n->name], "index assignment");
        const long long index = asNumberValue(evalNode(n->index), "array index");
        if (index < 0 || static_cast<size_t>(index) >= array->values.size()) throw std::runtime_error("E_INDEX_OUT_OF_RANGE:" + n->name);
        WapiValue val = evalNode(n->value);
        array->values[static_cast<size_t>(index)] = val;
        return val;
    }
    if (auto n = std::dynamic_pointer_cast<BlockStatement>(node)) {
        WapiValue last = 0;
        for (auto& stmt : n->statements) last = evalNode(stmt);
        return last;
    }
    if (auto n = std::dynamic_pointer_cast<IfStatement>(node)) {
        if (isTruthy(evalNode(n->condition))) return evalNode(n->thenBranch);
        if (n->elseBranch) return evalNode(n->elseBranch);
        return 0;
    }
    if (auto n = std::dynamic_pointer_cast<WhileStatement>(node)) {
        const int maxIterations = options.maxSteps;
        WapiValue last = 0;
        int iterations = 0;
        while (isTruthy(evalNode(n->condition))) {
            if (++iterations > maxIterations) throw std::runtime_error("E_LOOP_LIMIT: while exceeded " + std::to_string(maxIterations) + " iterations");
            try {
                last = evalNode(n->body);
            }
            catch (const ContinueSignal&) {
                continue;
            }
            catch (const BreakSignal&) {
                break;
            }
        }
        return last;
    }
    if (auto n = std::dynamic_pointer_cast<ForRangeStatement>(node)) {
        const long long start = asNumberValue(evalNode(n->start), "range start");
        const long long end = asNumberValue(evalNode(n->end), "range end");
        const long long step = n->step ? asNumberValue(evalNode(n->step), "range step") : 1;
        if (step == 0) throw std::runtime_error("E_RANGE_STEP_ZERO");
        WapiValue last = 0;
        int iterations = 0;
        for (long long i = start; step > 0 ? i < end : i > end; i += step) {
            if (++iterations > options.maxSteps) throw std::runtime_error("E_LOOP_LIMIT: for exceeded " + std::to_string(options.maxSteps) + " iterations");
            variables[n->variable] = (i >= (std::numeric_limits<int>::min)() && i <= (std::numeric_limits<int>::max)()) ? WapiValue(static_cast<int>(i)) : WapiValue(i);
            try {
                last = evalNode(n->body);
            }
            catch (const ContinueSignal&) {
                continue;
            }
            catch (const BreakSignal&) {
                break;
            }
        }
        return last;
    }
    if (std::dynamic_pointer_cast<BreakStatement>(node)) throw BreakSignal{};
    if (std::dynamic_pointer_cast<ContinueStatement>(node)) throw ContinueSignal{};
    if (auto n = std::dynamic_pointer_cast<ReturnStatement>(node)) {
        throw ReturnSignal{ n->value ? evalNode(n->value) : WapiValue(0) };
    }
    if (auto n = std::dynamic_pointer_cast<FunctionDeclaration>(node)) {
        userFunctions[n->name] = n;
        return 0;
    }
    if (auto n = std::dynamic_pointer_cast<StructDeclaration>(node)) {
        structRegistry[n->name] = n;
        return 0;
    }
    if (auto n = std::dynamic_pointer_cast<TryCatchStatement>(node)) {
        try {
            return evalNode(n->tryBlock);
        }
        catch (const std::exception& error) {
            if (std::string(error.what()).rfind("E_TIMEOUT", 0) == 0) throw;
            if (!n->errorName.empty()) variables[n->errorName] = std::string(error.what());
            return evalNode(n->catchBlock);
        }
    }
    if (std::dynamic_pointer_cast<IncludeStatement>(node)) return 0;
    if (auto n = std::dynamic_pointer_cast<StructLiteral>(node)) return evalStructLiteral(*n);
    if (auto n = std::dynamic_pointer_cast<MatchStatement>(node)) return evalMatchStatement(*n);
    if (auto n = std::dynamic_pointer_cast<FieldAssignment>(node)) return evalFieldAssignment(*n);
    if (auto n = std::dynamic_pointer_cast<FieldAccessExpression>(node)) return evalFieldAccess(*n);
    if (auto n = std::dynamic_pointer_cast<NullCoalesceExpression>(node)) {
        WapiValue left = evalNode(n->left);
        return isTruthy(left) ? left : evalNode(n->right);
    }
    if (auto n = std::dynamic_pointer_cast<MethodCallExpression>(node)) return evalMethodCall(*n);
    if (auto n = std::dynamic_pointer_cast<NullSafeCallExpression>(node)) return evalNullSafeCall(*n);
    if (auto n = std::dynamic_pointer_cast<IndexExpression>(node)) return evalIndexExpression(*n);
    if (auto n = std::dynamic_pointer_cast<TernaryExpression>(node)) return isTruthy(evalNode(n->condition)) ? evalNode(n->whenTrue) : evalNode(n->whenFalse);
    if (auto n = std::dynamic_pointer_cast<UnaryExpression>(node)) return evalUnaryExpression(*n);
    if (auto n = std::dynamic_pointer_cast<BinaryExpression>(node)) return evalBinaryExpression(*n);
    if (auto n = std::dynamic_pointer_cast<FunctionCall>(node)) return evalFunctionCall(n);
    throw std::runtime_error("E_UNKNOWN_NODE: unsupported AST node");
}
WapiValue Evaluator::evalStructLiteral(const StructLiteral& literal) {
    auto found = structRegistry.find(literal.typeName);
    if (found == structRegistry.end()) throw std::runtime_error("E_UNKNOWN_STRUCT:" + literal.typeName);
    auto instance = std::make_shared<WapiStructInstance>();
    instance->typeName = literal.typeName;
    for (const auto& field : found->second->fields) instance->fields[field.name] = std::monostate{};
    for (const auto& arg : literal.fields) {
        auto declared = std::find_if(found->second->fields.begin(), found->second->fields.end(), [&](const StructField& field) { return field.name == arg.name; });
        if (declared == found->second->fields.end()) throw std::runtime_error("E_STRUCT_FIELD:" + literal.typeName + "." + arg.name);
        WapiValue value = evalNode(arg.value);
        if (!typeMatches(declared->type, value)) throw std::runtime_error("E_TYPE:" + literal.typeName + "." + arg.name + " expected " + declared->type);
        instance->fields[arg.name] = value;
    }
    return instance;
}
WapiValue Evaluator::evalFieldAccess(const FieldAccessExpression& expr) {
    WapiValue target = evalNode(expr.target);
    if (auto p = std::get_if<WapiStructPtr>(&target)) {
        if (!*p) throw std::runtime_error("E_NULL_FIELD:" + expr.field);
        auto field = (*p)->fields.find(expr.field);
        if (field == (*p)->fields.end()) throw std::runtime_error("E_STRUCT_FIELD:" + (*p)->typeName + "." + expr.field);
        return field->second;
    }
    throw std::runtime_error("E_TYPE:field access expected struct");
}
WapiValue Evaluator::evalFieldAssignment(const FieldAssignment& stmt) {
    WapiValue target = evalNode(stmt.target);
    if (!std::holds_alternative<WapiStructPtr>(target) || !std::get<WapiStructPtr>(target)) throw std::runtime_error("E_TYPE:field assignment expected struct");
    auto instance = std::get<WapiStructPtr>(target);
    auto field = instance->fields.find(stmt.field);
    if (field == instance->fields.end()) throw std::runtime_error("E_STRUCT_FIELD:" + instance->typeName + "." + stmt.field);
    WapiValue value = evalNode(stmt.value);
    if (stmt.op != "=") {
        const long long lhs = asNumberValue(field->second, "field assignment");
        const long long rhs = asNumberValue(value, "field assignment");
        const std::string op = stmt.op.substr(0, 1);
        long long result = lhs;
        if (op == "+") result = lhs + rhs;
        else if (op == "-") result = lhs - rhs;
        else if (op == "*") result = lhs * rhs;
        else if (op == "/") { if (rhs == 0) throw std::runtime_error("E_DIVIDE_BY_ZERO"); result = lhs / rhs; }
        else if (op == "%") { if (rhs == 0) throw std::runtime_error("E_DIVIDE_BY_ZERO"); result = lhs % rhs; }
        else throw std::runtime_error("E_UNKNOWN_OPERATOR:" + stmt.op);
        value = result >= (std::numeric_limits<int>::min)() && result <= (std::numeric_limits<int>::max)() ? WapiValue(static_cast<int>(result)) : WapiValue(result);
    }
    field->second = value;
    return value;
}
WapiValue Evaluator::evalMethodCall(const MethodCallExpression& expr) {
    WapiValue target = evalNode(expr.target);
    std::vector<WapiValue> args;
    for (auto& arg : expr.args) args.push_back(evalNode(arg));
    const std::string& method = expr.method;
    if (auto text = std::get_if<std::string>(&target)) {
        if (method == "len" || method == "size") return static_cast<int>(text->size());
        if (method == "trim") return trimAscii(*text);
        if (method == "toLower") return toLowerAscii(*text);
        if (method == "toUpper") { std::string out = *text; std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); }); return out; }
        if (method == "contains") { if (args.size() != 1) throw std::runtime_error("E_ARG_COUNT:contains"); return text->find(valueToString(args[0])) != std::string::npos; }
        if (method == "startsWith") { if (args.size() != 1) throw std::runtime_error("E_ARG_COUNT:startsWith"); const std::string needle = valueToString(args[0]); return text->rfind(needle, 0) == 0; }
        if (method == "endsWith") { if (args.size() != 1) throw std::runtime_error("E_ARG_COUNT:endsWith"); const std::string needle = valueToString(args[0]); return text->size() >= needle.size() && text->compare(text->size() - needle.size(), needle.size(), needle) == 0; }
    }
    if (auto arrayPtr = std::get_if<WapiArrayPtr>(&target)) {
        auto array = *arrayPtr;
        if (!array) throw std::runtime_error("E_NULL_ARRAY:" + method);
        if (method == "len" || method == "size") return static_cast<int>(array->values.size());
        if (method == "push") { if (args.size() != 1) throw std::runtime_error("E_ARG_COUNT:push"); array->values.push_back(args[0]); return array; }
        if (method == "pop") { if (!args.empty()) throw std::runtime_error("E_ARG_COUNT:pop"); if (array->values.empty()) throw std::runtime_error("E_EMPTY_ARRAY:pop"); WapiValue value = array->values.back(); array->values.pop_back(); return value; }
        if (method == "first") { if (array->values.empty()) throw std::runtime_error("E_EMPTY_ARRAY:first"); return array->values.front(); }
        if (method == "last") { if (array->values.empty()) throw std::runtime_error("E_EMPTY_ARRAY:last"); return array->values.back(); }
        if (method == "sort") { std::sort(array->values.begin(), array->values.end(), [this](const WapiValue& a, const WapiValue& b) { return valueToString(a) < valueToString(b); }); return array; }
        if (method == "contains") { if (args.size() != 1) throw std::runtime_error("E_ARG_COUNT:contains"); return std::any_of(array->values.begin(), array->values.end(), [&](const WapiValue& value) { return valuesEqual(value, args[0]); }); }
    }
    throw std::runtime_error("E_UNKNOWN_METHOD:" + method);
}
WapiValue Evaluator::evalNullSafeCall(const NullSafeCallExpression& expr) {
    WapiValue target = evalNode(expr.target);
    if (!isTruthy(target)) return std::monostate{};
    MethodCallExpression call;
    call.target = expr.target;
    call.method = expr.method;
    call.args = expr.args;
    call.namedArgs = expr.namedArgs;
    return evalMethodCall(call);
}
WapiValue Evaluator::evalMatchStatement(const MatchStatement& stmt) {
    WapiValue subject = evalNode(stmt.subject);
    for (const auto& arm : stmt.arms) {
        bool matched = false;
        std::string boundName;
        bool hadOuter = false;
        WapiValue outerValue;
        if (arm.pattern.kind == "default") matched = true;
        else if (arm.pattern.kind == "binding") { matched = true; boundName = arm.pattern.text; auto outer = variables.find(boundName); hadOuter = outer != variables.end(); if (hadOuter) outerValue = outer->second; variables[boundName] = subject; }
        else if (arm.pattern.kind == "literal") matched = valuesEqual(subject, evalNode(arm.pattern.value));
        if (matched && (!arm.guard || isTruthy(evalNode(arm.guard)))) return evalNode(arm.body);
        if (!boundName.empty()) { if (hadOuter) variables[boundName] = outerValue; else variables.erase(boundName); }
    }
    return std::monostate{};
}
WapiValue Evaluator::evalUnaryExpression(const UnaryExpression& expr) {
    WapiValue value = evalNode(expr.value);
    if (expr.op == "-") {
        if (auto p = std::get_if<double>(&value)) return -*p;
        const long long number = -asNumberValue(value, "unary '-'");
        if (std::holds_alternative<int>(value) &&
            number >= (std::numeric_limits<int>::min)() &&
            number <= (std::numeric_limits<int>::max)()) {
            return static_cast<int>(number);
        }
        return number;
    }
    if (expr.op == "!") return !isTruthy(value);
    if (expr.op == "~") {
        const long long number = ~asNumberValue(value, "unary '~'");
        if (number >= (std::numeric_limits<int>::min)() && number <= (std::numeric_limits<int>::max)()) return static_cast<int>(number);
        return number;
    }
    throw std::runtime_error("E_UNKNOWN_OPERATOR:" + expr.op);
}
WapiValue Evaluator::evalBinaryExpression(const BinaryExpression& expr) {
    if (expr.op == "&&") return isTruthy(evalNode(expr.left)) && isTruthy(evalNode(expr.right));
    if (expr.op == "||") return isTruthy(evalNode(expr.left)) || isTruthy(evalNode(expr.right));
    WapiValue left = evalNode(expr.left);
    WapiValue right = evalNode(expr.right);
    if (expr.op == "==") return valuesEqual(left, right);
    if (expr.op == "!=") return !valuesEqual(left, right);
    if (expr.op == "+" && (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right))) {
        return valueToString(left) + valueToString(right);
    }
    const bool useDouble = std::holds_alternative<double>(left) || std::holds_alternative<double>(right);
    if (useDouble && expr.op != "%" && expr.op != "&" && expr.op != "|" && expr.op != "^" && expr.op != "<<" && expr.op != ">>") {
        const double lhs = numericDoubleValue(left);
        const double rhs = numericDoubleValue(right);
        if (expr.op == "<") return lhs < rhs;
        if (expr.op == "<=") return lhs <= rhs;
        if (expr.op == ">") return lhs > rhs;
        if (expr.op == ">=") return lhs >= rhs;
        if (expr.op == "+") return lhs + rhs;
        if (expr.op == "-") return lhs - rhs;
        if (expr.op == "*") return lhs * rhs;
        if (expr.op == "/") {
            if (rhs == 0.0) throw std::runtime_error("E_DIVIDE_BY_ZERO");
            return lhs / rhs;
        }
        throw std::runtime_error("E_UNKNOWN_OPERATOR:" + expr.op);
    }
    const long long lhs = asNumberValue(left, "operator '" + expr.op + "'");
    const long long rhs = asNumberValue(right, "operator '" + expr.op + "'");
    if (expr.op == "<") return lhs < rhs;
    if (expr.op == "<=") return lhs <= rhs;
    if (expr.op == ">") return lhs > rhs;
    if (expr.op == ">=") return lhs >= rhs;
    long long result = 0;
    if (expr.op == "+") result = lhs + rhs;
    else if (expr.op == "-") result = lhs - rhs;
    else if (expr.op == "*") result = lhs * rhs;
    else if (expr.op == "/") {
        if (rhs == 0) throw std::runtime_error("E_DIVIDE_BY_ZERO");
        result = lhs / rhs;
    }
    else if (expr.op == "%") {
        if (rhs == 0) throw std::runtime_error("E_DIVIDE_BY_ZERO");
        result = lhs % rhs;
    }
    else if (expr.op == "&") result = lhs & rhs;
    else if (expr.op == "|") result = lhs | rhs;
    else if (expr.op == "^") result = lhs ^ rhs;
    else if (expr.op == "<<") result = lhs << rhs;
    else if (expr.op == ">>") result = lhs >> rhs;
    else throw std::runtime_error("E_UNKNOWN_OPERATOR:" + expr.op);
    const bool promoteToLong = std::holds_alternative<long long>(left) || std::holds_alternative<long long>(right);
    if (!promoteToLong && result >= (std::numeric_limits<int>::min)() && result <= (std::numeric_limits<int>::max)()) return static_cast<int>(result);
    return result;
}
WapiValue Evaluator::evalIndexExpression(const IndexExpression& expr) {
    WapiValue target = evalNode(expr.target);
    const long long index = asNumberValue(evalNode(expr.index), "index");
    if (index < 0) throw std::runtime_error("E_INDEX_OUT_OF_RANGE");
    if (auto p = std::get_if<std::string>(&target)) {
        if (static_cast<size_t>(index) >= p->size()) throw std::runtime_error("E_INDEX_OUT_OF_RANGE");
        return std::string(1, (*p)[static_cast<size_t>(index)]);
    }
    auto array = asArrayValue(target, "index");
    if (static_cast<size_t>(index) >= array->values.size()) throw std::runtime_error("E_INDEX_OUT_OF_RANGE");
    return array->values[static_cast<size_t>(index)];
}
void Evaluator::checkArgCount(const std::shared_ptr<FunctionCall>& call, size_t expected) {
    if (call->args.size() != expected) {
        throw std::runtime_error(
            "E_ARG_COUNT:" + call->name + " expected=" + std::to_string(expected) +
            " got=" + std::to_string(call->args.size())
        );
    }
}
std::string Evaluator::modeToString() const {
    switch (options.mode) {
    case WapiMode::Safe: return "safe";
    case WapiMode::Dev: return "dev";
    case WapiMode::Unsafe: return "unsafe";
    }
    return "safe";
}
std::string Evaluator::nowIso8601() const {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &t);
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
void Evaluator::emitJsonEvent(const std::string& kind, const std::string& payload) const {
    if (!options.jsonOutput) return;
    std::cout << "{\"kind\":\"" << jsonEscape(kind) << "\",\"data\":" << payload << "}" << "\n";
}
void Evaluator::emitAudit(const std::string& functionName, const std::string& capability, const std::string& result, const std::string& detail) const {
    if (options.quiet) return;
    std::cout
        << "[WAPI_AUDIT]"
        << " ts=" << nowIso8601()
        << " mode=" << modeToString()
        << " checkOnly=" << (options.checkOnly ? "true" : "false")
        << " function=" << functionName
        << " capability=" << capability
        << " result=" << result;
    if (!detail.empty()) {
        std::cout << " detail=\"" << detail << "\"";
    }
    std::cout << "\n";
}
void Evaluator::enforcePolicy(const std::string& functionName, const std::string& capability, bool requiresInjectionFlag) const {
    const bool hasCapability = options.capabilities.count(capability) > 0;
    static const std::unordered_set<std::string> devOnlyCapabilities = {
        "proc.open.all_access", "proc.terminate", "proc.suspend", "proc.resume",
        "mem.read", "mem.write", "mem.alloc", "mem.free", "mem.protect", "mem.query",
        "thread.open", "thread.suspend", "thread.resume", "thread.context.write",
        "debug.attach", "debug.registers", "token.open", "token.privilege",
        "window.message", "inject.dll", "inject.shellcode"
    };
    static const std::unordered_set<std::string> softMissingCapabilities = {
        "runtime.print", "language.core", "language.string", "language.math", "language.array", "runtime.sleep"
    };
    if (devOnlyCapabilities.count(capability) && options.mode == WapiMode::Safe) {
        emitAudit(functionName, capability, "deny", "capability requires dev or unsafe mode");
        throw std::runtime_error("E_PERMISSION:" + functionName + " requires --mode dev|unsafe for capability=" + capability);
    }
    if (requiresInjectionFlag && options.mode != WapiMode::Unsafe && !options.allowInjection) {
        emitAudit(functionName, capability, "deny", "--allow-injection is required outside unsafe mode");
        throw std::runtime_error("E_PERMISSION:" + functionName + " requires --allow-injection");
    }
    if (hasCapability) {
        emitAudit(functionName, capability, "allow", "capability_present");
        return;
    }
    const bool hardMissingCapability = options.strictPermissions || softMissingCapabilities.count(capability) == 0;
    if (hardMissingCapability) {
        const std::string detail = options.strictPermissions ? "missing capability in strict mode" : "missing sensitive capability";
        emitAudit(functionName, capability, "deny", detail);
        throw std::runtime_error("E_PERMISSION:" + functionName + " missing capability=" + capability);
    }
    emitAudit(functionName, capability, "warn", "missing non-sensitive capability allowed for compatibility");
    if (!options.quiet) std::cout << "[WAPI_WARNING][E_PERMISSION_SOFT] " << functionName
              << " missing capability '" << capability
              << "' (allowed for non-sensitive compatibility)\n";
}
[[noreturn]] void Evaluator::throwArgType(const std::string& functionName, int argIndex, const std::string& expectedType) const {
    throw std::runtime_error(
        "E_ARG_TYPE:" + functionName + " arg=" + std::to_string(argIndex) + " expected=" + expectedType
    );
}
std::string Evaluator::asString(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex) {
    WapiValue v = evalNode(node);
    if (auto p = std::get_if<std::string>(&v)) return *p;
    throwArgType(functionName, argIndex, "string");
}
int Evaluator::asInt(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex) {
    WapiValue v = evalNode(node);
    if (auto p = std::get_if<int>(&v)) return *p;
    throwArgType(functionName, argIndex, "int");
}
long long Evaluator::asLongLong(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex) {
    WapiValue v = evalNode(node);
    if (auto p = std::get_if<long long>(&v)) return *p;
    if (auto pInt = std::get_if<int>(&v)) return static_cast<long long>(*pInt);
    throwArgType(functionName, argIndex, "long|int");
}
void* Evaluator::requireTrackedHandle(long long handleValue, const std::string& functionName) {
    if (handleValue == 0) {
        throw std::runtime_error("E_HANDLE_INVALID:" + functionName + " handle=0");
    }
    if (!trackedHandles.count(handleValue)) {
        throw std::runtime_error("E_HANDLE_UNKNOWN:" + functionName + " handle_not_tracked");
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(handleValue));
}
void Evaluator::releaseTrackedAllocations(long long handleValue) noexcept {
    auto found = trackedAllocations.find(handleValue);
    if (found == trackedAllocations.end()) return;
    if (!options.checkOnly) {
        HANDLE hProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue));
        for (long long address : found->second) {
            if (address > 0) {
                VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE);
            }
        }
    }
    trackedAllocations.erase(found);
}
void Evaluator::closeTrackedHandle(long long handleValue) noexcept {
    if (!trackedHandles.count(handleValue)) {
        trackedAllocations.erase(handleValue);
        return;
    }
    releaseTrackedAllocations(handleValue);
    if (!options.checkOnly) {
        HANDLE hProcess = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue));
        CloseHandle(hProcess);
    }
    trackedHandles.erase(handleValue);
}
void Evaluator::cleanupTrackedResources() noexcept {
    std::vector<long long> handles(trackedHandles.begin(), trackedHandles.end());
    for (long long handleValue : handles) {
        closeTrackedHandle(handleValue);
    }
    trackedHandles.clear();
    trackedAllocations.clear();
}
WapiValue Evaluator::evalUserFunction(const std::shared_ptr<FunctionDeclaration>& declaration, const std::shared_ptr<FunctionCall>& call) {
    if (call->args.size() != declaration->params.size()) {
        throw std::runtime_error("E_ARG_COUNT:" + call->name + " expected=" + std::to_string(declaration->params.size()) + " got=" + std::to_string(call->args.size()));
    }
    std::vector<WapiValue> args(declaration->params.size(), std::monostate{});
    std::vector<bool> assigned(declaration->params.size(), false);
    const size_t positionalCount = call->namedArgs.empty() ? call->args.size() : call->args.size() - call->namedArgs.size();
    for (size_t i = 0; i < positionalCount; ++i) { args[i] = evalNode(call->args[i]); assigned[i] = true; }
    for (const auto& named : call->namedArgs) {
        auto param = std::find_if(declaration->params.begin(), declaration->params.end(), [&](const auto& item) { return item.second == named.name; });
        if (param == declaration->params.end()) throw std::runtime_error("E_UNKNOWN_ARG:" + call->name + "." + named.name);
        const size_t index = static_cast<size_t>(std::distance(declaration->params.begin(), param));
        if (assigned[index]) throw std::runtime_error("E_DUP_ARG:" + call->name + "." + named.name);
        args[index] = evalNode(named.value);
        assigned[index] = true;
    }
    for (size_t i = 0; i < assigned.size(); ++i) if (!assigned[i]) throw std::runtime_error("E_ARG_MISSING:" + call->name + "." + declaration->params[i].second);
    auto outerVariables = variables;
    for (size_t i = 0; i < declaration->params.size(); ++i) {
        const auto& [type, name] = declaration->params[i];
        if (!typeMatches(type, args[i])) throw std::runtime_error("E_TYPE:" + call->name + " arg=" + std::to_string(i) + " expected " + type);
        variables[name] = args[i];
    }
    WapiValue result = 0;
    try { evalNode(declaration->body); }
    catch (const ReturnSignal& returned) { result = returned.value; }
    variables = outerVariables;
    if (!typeMatches(declaration->returnType, result) && !options.quiet) std::cout << "[WAPI_WARN] function " << call->name << " returned " << valueTypeName(result) << " but declared " << declaration->returnType << "\\n";
    return result;
}
WapiValue Evaluator::evalFunctionCall(std::shared_ptr<FunctionCall> call) {
    auto userFunction = userFunctions.find(call->name);
    if (userFunction != userFunctions.end()) return evalUserFunction(userFunction->second, call);
    if (call->name == "typeof") {
        checkArgCount(call, 1);
        return valueTypeName(evalNode(call->args[0]));
    }
    if (call->name == "assert") {
        if (call->args.empty() || call->args.size() > 2) throw std::runtime_error("E_ARG_COUNT:assert expected=1..2 got=" + std::to_string(call->args.size()));
        if (!isTruthy(evalNode(call->args[0]))) {
            const std::string message = call->args.size() == 2 ? asString(call->args[1], call->name, 1) : "assertion failed";
            throw std::runtime_error("E_ASSERT:" + message);
        }
        return true;
    }
    if (call->name == "toHex") {
        checkArgCount(call, 1);
        std::ostringstream oss;
        oss << std::hex << asNumberValue(evalNode(call->args[0]), "toHex");
        return oss.str();
    }
    if (call->name == "fromHex") {
        checkArgCount(call, 1);
        const std::string value = asString(call->args[0], call->name, 0);
        return static_cast<long long>(std::stoll(value, nullptr, 16));
    }
    if (call->name == "split") {
        checkArgCount(call, 2);
        const std::string value = asString(call->args[0], call->name, 0);
        const std::string delimiter = asString(call->args[1], call->name, 1);
        auto array = std::make_shared<WapiArray>();
        if (delimiter.empty()) {
            for (char ch : value) array->values.push_back(std::string(1, ch));
            return array;
        }
        size_t start = 0;
        size_t at = 0;
        while ((at = value.find(delimiter, start)) != std::string::npos) {
            array->values.push_back(value.substr(start, at - start));
            start = at + delimiter.size();
        }
        array->values.push_back(value.substr(start));
        return array;
    }
    if (call->name == "trim") {
        checkArgCount(call, 1);
        return trimAscii(asString(call->args[0], call->name, 0));
    }
    if (call->name == "padLeft" || call->name == "padRight") {
        if (call->args.size() < 2 || call->args.size() > 3) throw std::runtime_error("E_ARG_COUNT:" + call->name + " expected=2..3 got=" + std::to_string(call->args.size()));
        std::string value = asString(call->args[0], call->name, 0);
        const int width = asInt(call->args[1], call->name, 1);
        const std::string fillText = call->args.size() == 3 ? asString(call->args[2], call->name, 2) : " ";
        const char fill = fillText.empty() ? ' ' : fillText[0];
        if (width <= static_cast<int>(value.size())) return value;
        const std::string padding(static_cast<size_t>(width) - value.size(), fill);
        return call->name == "padLeft" ? padding + value : value + padding;
    }
    if (call->name == "sort") {
        checkArgCount(call, 1);
        WapiValue value = evalNode(call->args[0]);
        auto array = asArrayValue(value, "sort");
        std::sort(array->values.begin(), array->values.end(), [this](const WapiValue& a, const WapiValue& b) {
            return valueToString(a) < valueToString(b);
        });
        return array;
    }    if (call->name == "len") {
        checkArgCount(call, 1);
        WapiValue value = evalNode(call->args[0]);
        if (auto p = std::get_if<std::string>(&value)) return static_cast<int>(p->size());
        return static_cast<int>(asArrayValue(value, "len")->values.size());
    }
    if (call->name == "substr") {
        checkArgCount(call, 3);
        const std::string value = asString(call->args[0], call->name, 0);
        const int start = asInt(call->args[1], call->name, 1);
        const int count = asInt(call->args[2], call->name, 2);
        if (start < 0 || count < 0 || static_cast<size_t>(start) > value.size()) throw std::runtime_error("E_INDEX_OUT_OF_RANGE:substr");
        return value.substr(static_cast<size_t>(start), static_cast<size_t>(count));
    }
    if (call->name == "contains") {
        checkArgCount(call, 2);
        return asString(call->args[0], call->name, 0).find(asString(call->args[1], call->name, 1)) != std::string::npos;
    }
    if (call->name == "replace") {
        checkArgCount(call, 3);
        std::string value = asString(call->args[0], call->name, 0);
        const std::string needle = asString(call->args[1], call->name, 1);
        const std::string replacement = asString(call->args[2], call->name, 2);
        if (needle.empty()) return value;
        size_t at = 0;
        while ((at = value.find(needle, at)) != std::string::npos) {
            value.replace(at, needle.size(), replacement);
            at += replacement.size();
        }
        return value;
    }
    if (call->name == "toLower") {
        checkArgCount(call, 1);
        return toLowerAscii(asString(call->args[0], call->name, 0));
    }
    if (call->name == "toInt") {
        checkArgCount(call, 1);
        WapiValue value = evalNode(call->args[0]);
        if (isNumericValue(value)) return static_cast<int>(numericValue(value));
        if (auto p = std::get_if<std::string>(&value)) return std::stoi(*p);
        throwArgType(call->name, 0, "string|number");
    }
    if (call->name == "abs") {
        checkArgCount(call, 1);
        const long long value = asNumberValue(evalNode(call->args[0]), "abs");
        const long long result = value < 0 ? -value : value;
        if (result <= (std::numeric_limits<int>::max)()) return static_cast<int>(result);
        return result;
    }
    if (call->name == "min" || call->name == "max") {
        checkArgCount(call, 2);
        const long long a = asNumberValue(evalNode(call->args[0]), call->name);
        const long long b = asNumberValue(evalNode(call->args[1]), call->name);
        const long long result = call->name == "min" ? (std::min)(a, b) : (std::max)(a, b);
        if (result >= (std::numeric_limits<int>::min)() && result <= (std::numeric_limits<int>::max)()) return static_cast<int>(result);
        return result;
    }
    if (call->name == "push") {
        checkArgCount(call, 2);
        WapiValue value = evalNode(call->args[0]);
        auto array = asArrayValue(value, "push");
        array->values.push_back(evalNode(call->args[1]));
        return array;
    }
    if (call->name == "pop") {
        checkArgCount(call, 1);
        WapiValue value = evalNode(call->args[0]);
        auto array = asArrayValue(value, "pop");
        if (array->values.empty()) throw std::runtime_error("E_EMPTY_ARRAY:pop");
        WapiValue popped = array->values.back();
        array->values.pop_back();
        return popped;
    }
    using FunctionInvoker = WapiValue(*)(Evaluator&, const std::shared_ptr<FunctionCall>&);
    struct FunctionBinding {
        size_t argCount;
        const char* capability;
        bool requiresInjectionFlag;
        FunctionInvoker invoke;
    };
    static const std::unordered_map<std::string, FunctionBinding> functions = {
        {
            "print",
            {1, "runtime.print", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                WapiValue value = evaluator.evalNode(call->args[0]);
                std::cout << evaluator.valueToString(value) << "\n";
                return value;
            }}
        },
        {
            "findProcessPID",
            {1, "proc.list", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_findProcessPID(evaluator.asString(call->args[0], call->name, 0));
            }}
        },
        {
            "listProcesses",
            {0, "proc.list", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>&) -> WapiValue {
                return evaluator.wapi_listProcesses();
            }}
        },
        {
            "openProcess",
            {1, "proc.open.all_access", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_openProcess(evaluator.asInt(call->args[0], call->name, 0));
            }}
        },
        {
            "terminateProcess",
            {1, "proc.terminate", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_terminateProcess(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "suspendProcess",
            {1, "proc.suspend", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_suspendProcess(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "resumeProcess",
            {1, "proc.resume", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_resumeProcess(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "closeProcess",
            {1, "proc.close", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_closeProcess(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "readMemory",
            {2, "mem.read", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_readMemory(
                    evaluator.asLongLong(call->args[0], call->name, 0),
                    evaluator.asLongLong(call->args[1], call->name, 1)
                );
            }}
        },
        {
            "writeMemory",
            {3, "mem.write", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_writeMemory(
                    evaluator.asLongLong(call->args[0], call->name, 0),
                    evaluator.asLongLong(call->args[1], call->name, 1),
                    evaluator.asInt(call->args[2], call->name, 2)
                );
            }}
        },
        {
            "allocMemory",
            {2, "mem.alloc", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_allocMemory(
                    evaluator.asLongLong(call->args[0], call->name, 0),
                    evaluator.asInt(call->args[1], call->name, 1)
                );
            }}
        },
        {
            "freeMemory",
            {2, "mem.free", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_freeMemory(
                    evaluator.asLongLong(call->args[0], call->name, 0),
                    evaluator.asLongLong(call->args[1], call->name, 1)
                );
            }}
        },
        {
            "listModules",
            {1, "proc.modules", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_listModules(evaluator.asInt(call->args[0], call->name, 0));
            }}
        },
        {
            "getModuleBase",
            {2, "proc.modules", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_getModuleBaseAddress(
                    evaluator.asInt(call->args[0], call->name, 0),
                    evaluator.asString(call->args[1], call->name, 1)
                );
            }}
        },
        {
            "getModuleBaseAddress",
            {2, "proc.modules", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_getModuleBaseAddress(
                    evaluator.asInt(call->args[0], call->name, 0),
                    evaluator.asString(call->args[1], call->name, 1)
                );
            }}
        },
        {
            "getModuleSize",
            {2, "proc.modules", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_getModuleSize(evaluator.asInt(call->args[0], call->name, 0), evaluator.asString(call->args[1], call->name, 1));
            }}
        },
        {
            "protectMemory",
            {4, "mem.protect", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_protectMemory(evaluator.asLongLong(call->args[0], call->name, 0), evaluator.asLongLong(call->args[1], call->name, 1), evaluator.asInt(call->args[2], call->name, 2), evaluator.asInt(call->args[3], call->name, 3));
            }}
        },
        {
            "queryMemory",
            {2, "mem.query", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_queryMemory(evaluator.asLongLong(call->args[0], call->name, 0), evaluator.asLongLong(call->args[1], call->name, 1));
            }}
        },
        {
            "listThreads",
            {1, "thread.list", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_listThreads(evaluator.asInt(call->args[0], call->name, 0));
            }}
        },
        {
            "openThread",
            {1, "thread.open", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_openThread(evaluator.asInt(call->args[0], call->name, 0));
            }}
        },
        {
            "suspendThread",
            {1, "thread.suspend", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_suspendThread(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "resumeThread",
            {1, "thread.resume", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_resumeThread(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "getThreadContext",
            {1, "debug.registers", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_getThreadContext(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "setThreadContext",
            {2, "thread.context.write", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_setThreadContext(evaluator.asLongLong(call->args[0], call->name, 0), evaluator.asLongLong(call->args[1], call->name, 1));
            }}
        },
        {
            "injectShellcode",
            {2, "inject.shellcode", true, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_injectShellcode(evaluator.asInt(call->args[0], call->name, 0), evaluator.asString(call->args[1], call->name, 1));
            }}
        },
        {
            "createRemoteThread",
            {3, "inject.shellcode", true, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_createRemoteThread(evaluator.asInt(call->args[0], call->name, 0), evaluator.asLongLong(call->args[1], call->name, 1), evaluator.asLongLong(call->args[2], call->name, 2));
            }}
        },
        {
            "listWindowsByPID",
            {1, "window.pid", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_listWindowsByPID(evaluator.asInt(call->args[0], call->name, 0));
            }}
        },
        {
            "findWindowByPID",
            {2, "window.pid", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_findWindowByPID(evaluator.asInt(call->args[0], call->name, 0), evaluator.asString(call->args[1], call->name, 1));
            }}
        },
        {
            "sendWindowMessage",
            {4, "window.message", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_sendWindowMessage(evaluator.asLongLong(call->args[0], call->name, 0), evaluator.asInt(call->args[1], call->name, 1), evaluator.asLongLong(call->args[2], call->name, 2), evaluator.asLongLong(call->args[3], call->name, 3));
            }}
        },
        {
            "debugAttach",
            {1, "debug.attach", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue { return evaluator.wapi_debugAttach(evaluator.asInt(call->args[0], call->name, 0)); }}
        },
        {
            "debugWaitEvent",
            {0, "debug.attach", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>&) -> WapiValue { return evaluator.wapi_debugWaitEvent(); }}
        },
        {
            "debugReadRegisters",
            {1, "debug.registers", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue { return evaluator.wapi_debugReadRegisters(evaluator.asInt(call->args[0], call->name, 0)); }}
        },
        {
            "debugContinue",
            {1, "debug.attach", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue { return evaluator.wapi_debugContinue(evaluator.asInt(call->args[0], call->name, 0)); }}
        },
        {
            "openProcessToken",
            {1, "token.open", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue { return evaluator.wapi_openProcessToken(evaluator.asLongLong(call->args[0], call->name, 0)); }}
        },
        {
            "enablePrivilege",
            {1, "token.privilege", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue { return evaluator.wapi_enablePrivilege(evaluator.asString(call->args[0], call->name, 0)); }}
        },
        {
            "closeHandle",
            {1, "proc.handle.close", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_closeHandle(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        },
        {
            "findWindow",
            {1, "window.find", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_findWindow(evaluator.asString(call->args[0], call->name, 0));
            }}
        },
        {
            "injectDLL",
            {2, "inject.dll", true, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_injectDLL(
                    evaluator.asInt(call->args[0], call->name, 0),
                    evaluator.asString(call->args[1], call->name, 1)
                );
            }}
        },
        {
            "testInjectDLL",
            {1, "inject.dll", true, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_testInjectDLL(evaluator.asInt(call->args[0], call->name, 0));
            }}
        },
        {
            "sleep",
            {1, "runtime.sleep", false, [](Evaluator& evaluator, const std::shared_ptr<FunctionCall>& call) -> WapiValue {
                return evaluator.wapi_sleep(evaluator.asLongLong(call->args[0], call->name, 0));
            }}
        }
    };
    static const std::unordered_map<std::string, std::string> aliases = {
        {"proc.find", "findProcessPID"},
        {"proc.list", "listProcesses"},
        {"proc.open", "openProcess"},
        {"proc.terminate", "terminateProcess"},
        {"proc.suspend", "suspendProcess"},
        {"proc.resume", "resumeProcess"},
        {"proc.close", "closeProcess"},
        {"proc.modules", "listModules"},
        {"proc.moduleBase", "getModuleBaseAddress"},
        {"proc.module.base", "getModuleBaseAddress"},
        {"proc.module.size", "getModuleSize"},
        {"mem.protect", "protectMemory"},
        {"mem.query", "queryMemory"},
        {"thread.list", "listThreads"},
        {"thread.open", "openThread"},
        {"thread.suspend", "suspendThread"},
        {"thread.resume", "resumeThread"},
        {"thread.context", "getThreadContext"},
        {"thread.context.set", "setThreadContext"},
        {"window.listByPid", "listWindowsByPID"},
        {"window.findByPid", "findWindowByPID"},
        {"window.message", "sendWindowMessage"},
        {"inject.shellcode", "injectShellcode"},
        {"inject.thread", "createRemoteThread"},
        {"debug.attach", "debugAttach"},
        {"debug.wait", "debugWaitEvent"},
        {"debug.registers", "debugReadRegisters"},
        {"debug.continue", "debugContinue"},
        {"token.open", "openProcessToken"},
        {"token.privilege", "enablePrivilege"},
        {"handle.close", "closeHandle"},
        {"mem.read", "readMemory"},
        {"mem.write", "writeMemory"},
        {"mem.alloc", "allocMemory"},
        {"mem.free", "freeMemory"},
        {"window.find", "findWindow"},
        {"inject.dll", "injectDLL"},
        {"inject.testDll", "testInjectDLL"},
        {"inject.test", "testInjectDLL"},
        {"testInjectDll", "testInjectDLL"}
    };
    auto found = functions.find(call->name);
    if (found == functions.end()) {
        auto alias = aliases.find(call->name);
        if (alias != aliases.end()) found = functions.find(alias->second);
    }
    if (found == functions.end()) {
        throw std::runtime_error("E_UNKNOWN_FUNCTION:" + call->name);
    }
    const FunctionBinding& binding = found->second;
    checkArgCount(call, binding.argCount);
    enforcePolicy(call->name, binding.capability, binding.requiresInjectionFlag);
    return binding.invoke(*this, call);
}

WapiValue Evaluator::wapi_findProcessPID(const std::string& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) throw WapiUnstableException("Snapshot failed");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring target(name.begin(), name.end());
    target += L".exe";
    std::transform(target.begin(), target.end(), target.begin(), ::towlower);
    if (Process32FirstW(snap, &entry)) {
        do {
            std::wstring current(entry.szExeFile);
            std::transform(current.begin(), current.end(), current.begin(), ::towlower);
            if (current == target) {
                CloseHandle(snap);
                return static_cast<int>(entry.th32ProcessID);
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    throw WapiUnstableException("Process not found: " + name);
}
WapiValue Evaluator::wapi_listProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) throw WapiUnstableException("Snapshot failed");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::ostringstream json;
    bool firstJson = true;
    json << "[";
    if (Process32FirstW(snap, &entry)) {
        do {
            std::wstring ws(entry.szExeFile);
            std::string name = wideToUtf8(ws);
            std::cout << entry.th32ProcessID << " - " << name << "\n";
            if (!firstJson) json << ",";
            json << "{\"pid\":" << entry.th32ProcessID << ",\"name\":\"" << jsonEscape(name) << "\"}";
            firstJson = false;
        } while (Process32NextW(snap, &entry));
    }
    json << "]";
    emitJsonEvent("processes", json.str());
    CloseHandle(snap);
    return 0;
}
WapiValue Evaluator::wapi_openProcess(int pid) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (options.checkOnly) {
        emitAudit("openProcess", "proc.open.all_access", "allow", "checkOnly no side-effects");
        const long long synthetic = 1;
        trackedHandles.insert(synthetic);
        return synthetic;
    }
    HANDLE handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) throw WapiUnstableException("Failed to open process");
    long long tracked = static_cast<long long>(reinterpret_cast<uintptr_t>(handle));
    trackedHandles.insert(tracked);
    return tracked;
}
WapiValue Evaluator::wapi_terminateProcess(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "terminateProcess"));
    if (options.checkOnly) {
        emitAudit("terminateProcess", "proc.terminate", "allow", "checkOnly no side-effects");
        return 0;
    }
    if (!TerminateProcess(hProcess, 0)) throw WapiUnstableException("Failed to terminate process");
    closeTrackedHandle(handle);
    return 0;
}
WapiValue Evaluator::wapi_suspendProcess(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "suspendProcess"));
    if (options.checkOnly) {
        emitAudit("suspendProcess", "proc.suspend", "allow", "checkOnly no side-effects");
        return 0;
    }
    using NtSuspendProcessFunc = LONG(NTAPI*)(HANDLE);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) throw WapiUnstableException("Failed to get ntdll.dll");
    auto NtSuspendProcess = reinterpret_cast<NtSuspendProcessFunc>(GetProcAddress(ntdll, "NtSuspendProcess"));
    if (!NtSuspendProcess) throw WapiUnstableException("Failed to find NtSuspendProcess");
    LONG status = NtSuspendProcess(hProcess);
    if (status != 0) throw WapiUnstableException("NtSuspendProcess failed");
    return 0;
}
WapiValue Evaluator::wapi_resumeProcess(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "resumeProcess"));
    if (options.checkOnly) {
        emitAudit("resumeProcess", "proc.resume", "allow", "checkOnly no side-effects");
        return 0;
    }
    using NtResumeProcessFunc = LONG(NTAPI*)(HANDLE);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) throw WapiUnstableException("Failed to get ntdll.dll");
    auto NtResumeProcess = reinterpret_cast<NtResumeProcessFunc>(GetProcAddress(ntdll, "NtResumeProcess"));
    if (!NtResumeProcess) throw WapiUnstableException("Failed to find NtResumeProcess");
    LONG status = NtResumeProcess(hProcess);
    if (status != 0) throw WapiUnstableException("NtResumeProcess failed");
    return 0;
}
WapiValue Evaluator::wapi_closeProcess(long long handle) {
    requireTrackedHandle(handle, "closeProcess");
    if (options.checkOnly) {
        emitAudit("closeProcess", "proc.close", "allow", "checkOnly no side-effects");
        closeTrackedHandle(handle);
        return 0;
    }
    closeTrackedHandle(handle);
    return 0;
}
WapiValue Evaluator::wapi_readMemory(long long handle, long long address) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "readMemory"));
    if (address <= 0) throw WapiUnstableException("Invalid memory address");
    if (options.checkOnly) {
        emitAudit("readMemory", "mem.read", "allow", "checkOnly no side-effects");
        return 0;
    }
    int buffer = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &buffer, sizeof(buffer), &bytesRead)) {
        throw WapiUnstableException("Failed to read process memory");
    }
    std::cout << "Read " << bytesRead << " bytes from address 0x" << std::hex << address << ": " << std::dec << buffer << "\n";
    return buffer;
}
WapiValue Evaluator::wapi_writeMemory(long long handle, long long address, int value) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "writeMemory"));
    if (address <= 0) throw WapiUnstableException("Invalid memory address");
    if (options.checkOnly) {
        emitAudit("writeMemory", "mem.write", "allow", "checkOnly no side-effects");
        return 0;
    }
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), &value, sizeof(value), &bytesWritten)) {
        throw WapiUnstableException("Failed to write process memory");
    }
    std::cout << "Wrote " << bytesWritten << " bytes to address 0x" << std::hex << address << "\n";
    return 0;
}
WapiValue Evaluator::wapi_allocMemory(long long handle, int size) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "allocMemory"));
    if (size <= 0) throw WapiUnstableException("Invalid allocation size");
    if (options.checkOnly) {
        emitAudit("allocMemory", "mem.alloc", "allow", "checkOnly no side-effects");
        return static_cast<long long>(0x1000);
    }
    LPVOID addr = VirtualAllocEx(hProcess, nullptr, static_cast<SIZE_T>(size), MEM_COMMIT, PAGE_READWRITE);
    if (!addr) throw WapiUnstableException("Failed to allocate memory");
    const long long trackedAddress = static_cast<long long>(reinterpret_cast<uintptr_t>(addr));
    trackedAllocations[handle].insert(trackedAddress);
    std::cout << "Allocated " << size << " bytes\n";
    return trackedAddress;
}
WapiValue Evaluator::wapi_freeMemory(long long handle, long long address) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "freeMemory"));
    if (address <= 0) throw WapiUnstableException("Invalid memory address");
    if (options.checkOnly) {
        emitAudit("freeMemory", "mem.free", "allow", "checkOnly no side-effects");
        return 0;
    }
    if (!VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE)) {
        throw WapiUnstableException("Failed to free memory");
    }
    auto found = trackedAllocations.find(handle);
    if (found != trackedAllocations.end()) {
        found->second.erase(address);
        if (found->second.empty()) trackedAllocations.erase(found);
    }
    std::cout << "Freed memory\n";
    return 0;
}
WapiValue Evaluator::wapi_listModules(int pid) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (options.checkOnly) {
        emitAudit("listModules", "proc.modules", "allow", "checkOnly no side-effects");
        return 0;
    }
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        throw WapiUnstableException("Failed to create module snapshot");
    }
    MODULEENTRY32W me32{};
    me32.dwSize = sizeof(me32);
    if (!Module32FirstW(hSnapshot, &me32)) {
        CloseHandle(hSnapshot);
        throw WapiUnstableException("Failed to read first module");
    }
    std::ostringstream json;
    bool firstJson = true;
    json << "[";
    do {
        std::wstring moduleName(me32.szModule);
        std::wstring modulePath(me32.szExePath);
        const std::string name = wideToUtf8(moduleName);
        const std::string path = wideToUtf8(modulePath);
        const uintptr_t base = reinterpret_cast<uintptr_t>(me32.modBaseAddr);
        std::cout
            << me32.th32ProcessID << " - "
            << name
            << " @ 0x" << std::hex << base
            << " size=" << std::dec << me32.modBaseSize
            << " path=" << path
            << "\n";
        if (!firstJson) json << ",";
        json << "{\"pid\":" << me32.th32ProcessID << ",\"name\":\"" << jsonEscape(name) << "\",\"base\":" << base << ",\"size\":" << me32.modBaseSize << ",\"path\":\"" << jsonEscape(path) << "\"}";
        firstJson = false;
    } while (Module32NextW(hSnapshot, &me32));
    json << "]";
    emitJsonEvent("modules", json.str());
    CloseHandle(hSnapshot);
    return 0;
}
WapiValue Evaluator::wapi_getModuleBaseAddress(int pid, const std::string& moduleName) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (moduleName.empty()) throw WapiUnstableException("Module name is empty");
    if (options.checkOnly) {
        emitAudit("getModuleBaseAddress", "proc.modules", "check_only", "PID: " + std::to_string(pid));
        return static_cast<long long>(0);
    }
    std::wstring target(moduleName.begin(), moduleName.end());
    std::transform(target.begin(), target.end(), target.begin(), ::towlower);
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        emitAudit("getModuleBaseAddress", "proc.modules", "failure", "snapshot_failed_pid_" + std::to_string(pid));
        throw WapiUnstableException("Failed to create module snapshot");
    }
    MODULEENTRY32W modEntry{};
    modEntry.dwSize = sizeof(modEntry);
    if (Module32FirstW(hSnapshot, &modEntry)) {
        do {
            std::wstring current(modEntry.szModule);
            std::transform(current.begin(), current.end(), current.begin(), ::towlower);
            if (current == target) {
                const long long baseAddress = static_cast<long long>(reinterpret_cast<uintptr_t>(modEntry.modBaseAddr));
                CloseHandle(hSnapshot);
                emitAudit("getModuleBaseAddress", "proc.modules", "success", moduleName + " base located");
                return baseAddress;
            }
            std::wstring path(modEntry.szExePath);
            std::transform(path.begin(), path.end(), path.begin(), ::towlower);
            if (path.size() >= target.size() && path.compare(path.size() - target.size(), target.size(), target) == 0) {
                const long long baseAddress = static_cast<long long>(reinterpret_cast<uintptr_t>(modEntry.modBaseAddr));
                CloseHandle(hSnapshot);
                emitAudit("getModuleBaseAddress", "proc.modules", "success", moduleName + " base located");
                return baseAddress;
            }
        } while (Module32NextW(hSnapshot, &modEntry));
    }
    else {
        const DWORD error = GetLastError();
        CloseHandle(hSnapshot);
        emitAudit("getModuleBaseAddress", "proc.modules", "failure", "module_walk_failed_pid_" + std::to_string(pid));
        throw WapiUnstableException("Failed to read process modules, error=" + std::to_string(error));
    }
    CloseHandle(hSnapshot);
    emitAudit("getModuleBaseAddress", "proc.modules", "failure", "module_not_found_" + moduleName);
    throw std::runtime_error("E_MODULE_NOT_FOUND: " + moduleName + " in PID " + std::to_string(pid));
}
WapiValue Evaluator::wapi_getModuleSize(int pid, const std::string& moduleName) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (moduleName.empty()) throw WapiUnstableException("Module name is empty");
    if (options.checkOnly) {
        emitAudit("getModuleSize", "proc.modules", "allow", "checkOnly no side-effects");
        return 0;
    }
    std::wstring target(moduleName.begin(), moduleName.end());
    std::transform(target.begin(), target.end(), target.begin(), ::towlower);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) throw WapiUnstableException("Failed to create module snapshot");
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module)) {
        do {
            std::wstring current(module.szModule);
            std::transform(current.begin(), current.end(), current.begin(), ::towlower);
            if (current == target) {
                const int size = static_cast<int>(module.modBaseSize);
                CloseHandle(snapshot);
                return size;
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    throw std::runtime_error("E_MODULE_NOT_FOUND: " + moduleName + " in PID " + std::to_string(pid));
}
WapiValue Evaluator::wapi_protectMemory(long long handle, long long address, int size, int protection) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "protectMemory"));
    if (address <= 0 || size <= 0) throw WapiUnstableException("Invalid memory range");
    if (options.checkOnly) {
        emitAudit("protectMemory", "mem.protect", "allow", "checkOnly no side-effects");
        return 0;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), static_cast<SIZE_T>(size), static_cast<DWORD>(protection), &oldProtect)) {
        throw WapiUnstableException("Failed to change memory protection");
    }
    return static_cast<int>(oldProtect);
}
WapiValue Evaluator::wapi_queryMemory(long long handle, long long address) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "queryMemory"));
    if (address <= 0) throw WapiUnstableException("Invalid memory address");
    if (options.checkOnly) {
        emitAudit("queryMemory", "mem.query", "allow", "checkOnly no side-effects");
        return 0;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQueryEx(hProcess, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &info, sizeof(info)) == 0) {
        throw WapiUnstableException("Failed to query memory");
    }
    std::cout << "Base=0x" << std::hex << reinterpret_cast<uintptr_t>(info.BaseAddress)
              << " RegionSize=" << std::dec << info.RegionSize
              << " State=" << info.State << " Protect=" << info.Protect << " Type=" << info.Type << "\n";
    emitJsonEvent("memoryRegion", "{\"base\":" + std::to_string(reinterpret_cast<uintptr_t>(info.BaseAddress)) + ",\"size\":" + std::to_string(info.RegionSize) + ",\"state\":" + std::to_string(info.State) + ",\"protect\":" + std::to_string(info.Protect) + "}");
    return static_cast<long long>(info.RegionSize);
}
WapiValue Evaluator::wapi_listThreads(int pid) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (options.checkOnly) { emitAudit("listThreads", "thread.list", "allow", "checkOnly no side-effects"); return 0; }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw WapiUnstableException("Failed to create thread snapshot");
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do { if (entry.th32OwnerProcessID == static_cast<DWORD>(pid)) std::cout << entry.th32ThreadID << " - pid=" << pid << "\n"; }
        while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}
WapiValue Evaluator::wapi_openThread(int tid) {
    if (tid <= 0) throw WapiUnstableException("Invalid tid");
    if (options.checkOnly) { emitAudit("openThread", "thread.open", "allow", "checkOnly no side-effects"); trackedHandles.insert(2); return 2; }
    HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, static_cast<DWORD>(tid));
    if (!thread) throw WapiUnstableException("Failed to open thread");
    const long long tracked = static_cast<long long>(reinterpret_cast<uintptr_t>(thread));
    trackedHandles.insert(tracked);
    return tracked;
}
WapiValue Evaluator::wapi_suspendThread(long long threadHandle) {
    HANDLE thread = reinterpret_cast<HANDLE>(requireTrackedHandle(threadHandle, "suspendThread"));
    if (options.checkOnly) { emitAudit("suspendThread", "thread.suspend", "allow", "checkOnly no side-effects"); return 0; }
    DWORD count = SuspendThread(thread);
    if (count == static_cast<DWORD>(-1)) throw WapiUnstableException("Failed to suspend thread");
    return static_cast<int>(count);
}
WapiValue Evaluator::wapi_resumeThread(long long threadHandle) {
    HANDLE thread = reinterpret_cast<HANDLE>(requireTrackedHandle(threadHandle, "resumeThread"));
    if (options.checkOnly) { emitAudit("resumeThread", "thread.resume", "allow", "checkOnly no side-effects"); return 0; }
    DWORD count = ResumeThread(thread);
    if (count == static_cast<DWORD>(-1)) throw WapiUnstableException("Failed to resume thread");
    return static_cast<int>(count);
}
WapiValue Evaluator::wapi_getThreadContext(long long threadHandle) {
    HANDLE thread = reinterpret_cast<HANDLE>(requireTrackedHandle(threadHandle, "getThreadContext"));
    if (options.checkOnly) { emitAudit("getThreadContext", "debug.registers", "allow", "checkOnly no side-effects"); return 0; }
    CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &context)) throw WapiUnstableException("Failed to read thread context");
#if defined(_M_X64)
    return static_cast<long long>(context.Rip);
#elif defined(_M_IX86)
    return static_cast<long long>(context.Eip);
#elif defined(_M_ARM64)
    return static_cast<long long>(context.Pc);
#else
    #error Unsupported architecture
#endif
}
WapiValue Evaluator::wapi_setThreadContext(long long threadHandle, long long contextAddress) {
    HANDLE thread = reinterpret_cast<HANDLE>(requireTrackedHandle(threadHandle, "setThreadContext"));
    if (contextAddress <= 0) throw WapiUnstableException("Invalid context address");
    if (options.checkOnly) { emitAudit("setThreadContext", "thread.context.write", "allow", "checkOnly no side-effects"); return 0; }
    CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &context)) throw WapiUnstableException("Failed to read thread context");
#if defined(_M_X64)
    context.Rip = static_cast<DWORD64>(contextAddress);
#elif defined(_M_IX86)
    context.Eip = static_cast<DWORD>(contextAddress);
#elif defined(_M_ARM64)
    context.Pc = static_cast<DWORD64>(contextAddress);
#else
    #error Unsupported architecture
#endif
    if (!SetThreadContext(thread, &context)) throw WapiUnstableException("Failed to write thread context");
    return 0;
}
WapiValue Evaluator::wapi_closeHandle(long long handle) {
    requireTrackedHandle(handle, "closeHandle");
    if (options.checkOnly) {
        emitAudit("closeHandle", "proc.handle.close", "allow", "checkOnly no side-effects");
        closeTrackedHandle(handle);
        return 0;
    }
    closeTrackedHandle(handle);
    return 0;
}
WapiValue Evaluator::wapi_findWindow(const std::string& name) {
    if (options.checkOnly) {
        emitAudit("findWindow", "window.find", "allow", "checkOnly no side-effects");
    }
    HWND hwnd = FindWindowA(nullptr, name.c_str());
    if (!hwnd) return 0;
    return static_cast<long long>(reinterpret_cast<uintptr_t>(hwnd));
}
namespace {
struct WindowSearchState {
    DWORD pid = 0;
    std::string title;
    HWND found = nullptr;
    bool list = false;
};
BOOL CALLBACK enumWindowsForPid(HWND hwnd, LPARAM lparam) {
    auto* state = reinterpret_cast<WindowSearchState*>(lparam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != state->pid) return TRUE;
    char title[512]{};
    GetWindowTextA(hwnd, title, sizeof(title));
    if (state->list) {
        std::cout << reinterpret_cast<uintptr_t>(hwnd) << " - pid=" << state->pid << " title=\"" << title << "\"\n";
        return TRUE;
    }
    if (state->title.empty() || std::string(title).find(state->title) != std::string::npos) {
        state->found = hwnd;
        return FALSE;
    }
    return TRUE;
}
}
WapiValue Evaluator::wapi_listWindowsByPID(int pid) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (options.checkOnly) { emitAudit("listWindowsByPID", "window.pid", "allow", "checkOnly no side-effects"); return 0; }
    WindowSearchState state{ static_cast<DWORD>(pid), "", nullptr, true };
    EnumWindows(enumWindowsForPid, reinterpret_cast<LPARAM>(&state));
    return 0;
}
WapiValue Evaluator::wapi_findWindowByPID(int pid, const std::string& title) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (options.checkOnly) { emitAudit("findWindowByPID", "window.pid", "allow", "checkOnly no side-effects"); return 0; }
    WindowSearchState state{ static_cast<DWORD>(pid), title, nullptr, false };
    EnumWindows(enumWindowsForPid, reinterpret_cast<LPARAM>(&state));
    return static_cast<long long>(reinterpret_cast<uintptr_t>(state.found));
}
WapiValue Evaluator::wapi_sendWindowMessage(long long hwndValue, int message, long long wparam, long long lparam) {
    if (hwndValue <= 0) throw WapiUnstableException("Invalid window handle");
    if (options.checkOnly) { emitAudit("sendWindowMessage", "window.message", "allow", "checkOnly no side-effects"); return 0; }
    LRESULT result = SendMessageA(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndValue)), static_cast<UINT>(message), static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    return static_cast<long long>(result);
}
WapiValue Evaluator::wapi_debugAttach(int pid) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (options.checkOnly) { emitAudit("debugAttach", "debug.attach", "allow", "checkOnly no side-effects"); return 0; }
    if (!DebugActiveProcess(static_cast<DWORD>(pid))) throw WapiUnstableException("Failed to attach debugger");
    return 0;
}
WapiValue Evaluator::wapi_debugWaitEvent() {
    if (options.checkOnly) { emitAudit("debugWaitEvent", "debug.attach", "allow", "checkOnly no side-effects"); return 0; }
    DEBUG_EVENT event{};
    if (!WaitForDebugEvent(&event, 1000)) throw WapiUnstableException("No debug event received");
    std::cout << "Debug event code=" << event.dwDebugEventCode << " pid=" << event.dwProcessId << " tid=" << event.dwThreadId << "\n";
    return static_cast<int>(event.dwDebugEventCode);
}
WapiValue Evaluator::wapi_debugReadRegisters(int tid) {
    long long thread = numericValue(wapi_openThread(tid));
    WapiValue result = wapi_getThreadContext(thread);
    closeTrackedHandle(thread);
    return result;
}
WapiValue Evaluator::wapi_debugContinue(int eventCode) {
    if (options.checkOnly) { emitAudit("debugContinue", "debug.attach", "allow", "checkOnly no side-effects"); return 0; }
    if (!ContinueDebugEvent(eventCode, 0, 0)) {
        throw WapiUnstableException("Failed to continue debug event");
    }
    return 0;
}
WapiValue Evaluator::wapi_openProcessToken(long long handle) {
    HANDLE process = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "openProcessToken"));
    if (options.checkOnly) { emitAudit("openProcessToken", "token.open", "allow", "checkOnly no side-effects"); trackedHandles.insert(3); return 3; }
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) throw WapiUnstableException("Failed to open process token");
    const long long tracked = static_cast<long long>(reinterpret_cast<uintptr_t>(token));
    trackedHandles.insert(tracked);
    return tracked;
}
WapiValue Evaluator::wapi_enablePrivilege(const std::string& privilegeName) {
    if (privilegeName.empty()) throw WapiUnstableException("Privilege name is empty");
    if (options.checkOnly) { emitAudit("enablePrivilege", "token.privilege", "allow", "checkOnly no side-effects"); return 0; }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) throw WapiUnstableException("Failed to open current process token");
    TOKEN_PRIVILEGES privileges{};
    if (!LookupPrivilegeValueA(nullptr, privilegeName.c_str(), &privileges.Privileges[0].Luid)) { CloseHandle(token); throw WapiUnstableException("Failed to resolve privilege"); }
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) || GetLastError() == ERROR_NOT_ALL_ASSIGNED) { CloseHandle(token); throw WapiUnstableException("Failed to enable privilege"); }
    CloseHandle(token);
    return 0;
}
WapiValue Evaluator::wapi_createRemoteThread(int pid, long long startAddress, long long parameter) {
    if (pid <= 0 || startAddress <= 0) throw WapiUnstableException("Invalid remote thread arguments");
    if (options.checkOnly) { emitAudit("createRemoteThread", "inject.shellcode", "allow", "checkOnly no side-effects"); return 0; }
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(pid));
    if (!process) throw WapiUnstableException("Failed to open process");
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<uintptr_t>(startAddress)), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(parameter)), 0, nullptr);
    if (!thread) { CloseHandle(process); throw WapiUnstableException("Failed to create remote thread"); }
    const long long tracked = static_cast<long long>(reinterpret_cast<uintptr_t>(thread));
    trackedHandles.insert(tracked);
    CloseHandle(process);
    return tracked;
}
WapiValue Evaluator::wapi_injectShellcode(int pid, const std::string& hexBytes) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (hexBytes.empty()) throw WapiUnstableException("Shellcode is empty");
    if (options.checkOnly) { emitAudit("injectShellcode", "inject.shellcode", "allow", "checkOnly no side-effects"); return 0; }
    std::vector<unsigned char> shellcode;
    shellcode.reserve(hexBytes.size() / 2);
    for (size_t i = 0; i < hexBytes.size(); i += 2) {
        std::string byteString = hexBytes.substr(i, 2);
        unsigned char byte = static_cast<unsigned char>(std::stoul(byteString, nullptr, 16));
        shellcode.push_back(byte);
    }
    if (shellcode.empty()) {
        throw WapiUnstableException("Failed to decode shellcode - invalid hex string");
    }
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(pid));
    if (!hProcess) throw WapiUnstableException("Failed to open process for shellcode injection");
    LPVOID remote = VirtualAllocEx(hProcess, nullptr, shellcode.size(), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!remote) {
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to allocate memory for shellcode");
    }
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, remote, shellcode.data(), shellcode.size(), &bytesWritten) || bytesWritten != shellcode.size()) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to write shellcode to process memory");
    }
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote), nullptr, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to create remote thread for shellcode");
    }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);
    std::cout << "Shellcode injected successfully, exit code: " << exitCode << "\n";
    return static_cast<long long>(exitCode);
}
WapiValue Evaluator::wapi_injectDLL(int pid, const std::string& dllPath) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (dllPath.empty()) throw WapiUnstableException("DLL path is empty");
    if (!std::filesystem::exists(std::filesystem::path(dllPath))) {
        throw WapiUnstableException("DLL path does not exist: " + dllPath);
    }
    if (options.checkOnly) {
        emitAudit("injectDLL", "inject.dll", "allow", "checkOnly no side-effects");
        return 0;
    }
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(pid));
    if (!hProcess) throw WapiUnstableException("Failed to open process");
    LPVOID remote = VirtualAllocEx(hProcess, nullptr, dllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to allocate memory");
    }
    if (!WriteProcessMemory(hProcess, remote, dllPath.c_str(), dllPath.size() + 1, nullptr)) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to write memory");
    }
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA")), remote, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to create remote thread");
    }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);
    if (exitCode == 0) {
        throw WapiUnstableException("LoadLibrary failed inside target process - check DLL path");
    }
    std::cout << "DLL injected successfully\n";
    return 0;
}
WapiValue Evaluator::wapi_testInjectDLL(int pid) {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        throw WapiUnstableException("Failed to resolve Wapi executable path");
    }
    const std::filesystem::path exeDirectory = std::filesystem::path(exePath).parent_path();
    std::filesystem::path dllPath = exeDirectory / L"TestDLL.dll";
    if (!std::filesystem::exists(dllPath)) {
        const std::filesystem::path configuration = exeDirectory.filename();
        const std::filesystem::path platform = exeDirectory.parent_path().filename();
        const std::filesystem::path projectRoot = exeDirectory.parent_path().parent_path();
        const std::filesystem::path projectDll = projectRoot / L"TestDLL" / platform / configuration / L"TestDLL.dll";
        if (std::filesystem::exists(projectDll)) dllPath = projectDll;
    }
    if (!std::filesystem::exists(dllPath)) {
        throw WapiUnstableException("TestDLL.dll was not found beside Wapi.exe or in the matching TestDLL build output");
    }
    return wapi_injectDLL(pid, wideToUtf8(dllPath.wstring()));
}

WapiValue Evaluator::wapi_sleep(long long milliseconds) {
    if (milliseconds < 0) {
        throw WapiUnstableException("Sleep duration cannot be negative");
    }
    if (options.checkOnly) {
        emitAudit("sleep", "runtime.sleep", "allow", "checkOnly no side-effects");
        return 0;
    }
    ::Sleep(static_cast<DWORD>(milliseconds));
    return 0;
}
