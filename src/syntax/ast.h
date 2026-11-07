#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <variant>

// AST node definitions.

namespace cling {

// Source position, 1-based. line == 0 means unknown (synthesized nodes).
struct SourceLoc {
    int line = 0;
    int column = 0;
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual std::string toString() const = 0;

    SourceLoc loc;  // source position; line == 0 if unset
};

using ASTNodePtr = std::unique_ptr<ASTNode>;

class Expression : public ASTNode {
public:
    virtual ~Expression() = default;
};

using ExpressionPtr = std::unique_ptr<Expression>;

class Statement : public ASTNode {
public:
    virtual ~Statement() = default;

    // True for a declaration merged in from an #include'd .clg file so the analyses can
    // see it. It is not part of this file's source and must not be emitted again
    // (that would define it twice).
    bool isExternalDeclaration = false;

    // The source file this top-level statement was parsed from. Gives synthesized names
    // (closure classes) a stem that is the same whichever file is compiled.
    std::string originFile;
};

using StatementPtr = std::unique_ptr<Statement>;

// Expression implementations
class Identifier : public Expression {
public:
    std::string name;

    explicit Identifier(const std::string& n) : name(n) {}
    std::string toString() const override { return name; }
};

class Number : public Expression {
public:
    std::string original_text;

    explicit Number(const std::string& text) : original_text(text) {}
    std::string toString() const override { return original_text; }
};

// `this`, a keyword and not an Identifier named "this".
class ThisExpression : public Expression {
public:
    std::string toString() const override { return "this"; }
};

// A qualified name like `std::cout`, stored as the full joined text.
class ScopedIdentifier : public Expression {
public:
    std::string qualifiedName;

    explicit ScopedIdentifier(const std::string& name) : qualifiedName(name) {}
    std::string toString() const override { return qualifiedName; }
};

// A string literal; holds the original text including the quotes.
class StringLiteral : public Expression {
public:
    std::string original_text;

    explicit StringLiteral(const std::string& text) : original_text(text) {}
    std::string toString() const override { return original_text; }
};

// A char literal; holds the original text including the quotes.
class CharLiteral : public Expression {
public:
    std::string original_text;

    explicit CharLiteral(const std::string& text) : original_text(text) {}
    std::string toString() const override { return original_text; }
};

class BinaryExpression : public Expression {
public:
    ExpressionPtr left;
    std::string operator_str;
    ExpressionPtr right;

    BinaryExpression(ExpressionPtr l, const std::string& op, ExpressionPtr r)
        : left(std::move(l)), operator_str(op), right(std::move(r)) {}

    std::string toString() const override {
        return "(" + left->toString() + " " + operator_str + " " + right->toString() + ")";
    }
};

// A unary prefix expression, e.g. `!left` or `-x`.
class UnaryExpression : public Expression {
public:
    std::string operator_str;
    ExpressionPtr operand;

    UnaryExpression(const std::string& op, ExpressionPtr expr)
        : operator_str(op), operand(std::move(expr)) {}

    std::string toString() const override {
        return operator_str + operand->toString();
    }
};

class FunctionCall : public Expression {
public:
    std::string function_name;
    std::vector<ExpressionPtr> arguments;
    // For `recv.method(args)`, the `recv.method` MemberAccess; null for a plain call `f(args)`.
    ExpressionPtr callee;
    // Call-site type argument (`n.as<int>()` -> "int"), empty for an ordinary call.
    // Such a call resolves to a method specialization (see specializationKey in class_hierarchy.h).
    std::string templateArg;

    FunctionCall(const std::string& name, std::vector<ExpressionPtr> args)
        : function_name(name), arguments(std::move(args)) {}

    std::string toString() const override {
        std::string result = function_name;
        if (!templateArg.empty()) result += "<" + templateArg + ">";
        result += "(";
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i > 0) result += ", ";
            result += arguments[i]->toString();
        }
        result += ")";
        return result;
    }
};

// Statement implementations
class VariableDeclaration : public Statement {
public:
    std::string type;
    std::string identifier;
    ExpressionPtr expression;
    bool isObserver = false;  // true if declared with `ref` (non-owning observer)

    VariableDeclaration(const std::string& t, const std::string& id, ExpressionPtr expr,
                        bool observer = false)
        : type(t), identifier(id), expression(std::move(expr)), isObserver(observer) {}

    std::string toString() const override {
        return std::string(isObserver ? "ref " : "") + type + " " + identifier +
               " = " + expression->toString() + ";";
    }
};

// A container declared without an initializer, e.g. `std::vector<int> v;`, which starts
// empty. The one exception to definite initialization, since an empty container is safe
// to read. `type` is always a generic type such as "std::vector<int>".
class ContainerDeclaration : public Statement {
public:
    std::string type;
    std::string identifier;

    ContainerDeclaration(const std::string& t, const std::string& id)
        : type(t), identifier(id) {}

    std::string toString() const override {
        return type + " " + identifier + ";";
    }
};

class AssignmentStatement : public Statement {
public:
    // Bare-name target (`a = ...`): `variable` is the name, `target` is null.
    // Member target (`a.b = ...`): `target` is the MemberAccess, `variable` the root object (`a`).
    std::string variable;
    ExpressionPtr expression;
    ExpressionPtr target;  // non-null iff the LHS is a member access

    AssignmentStatement(const std::string& var, ExpressionPtr expr)
        : variable(var), expression(std::move(expr)) {}

    std::string toString() const override {
        std::string lhs = target ? target->toString() : variable;
        return lhs + " := " + expression->toString() + ";";
    }
};

// An expression used as a statement, e.g. `sink(a);`.
class ExpressionStatement : public Statement {
public:
    ExpressionPtr expression;

    explicit ExpressionStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return expression->toString() + ";";
    }
};

class CommentStatement : public Statement {
public:
    std::string content;

    explicit CommentStatement(const std::string& comment)
        : content(comment) {}

    std::string toString() const override {
        return "//" + content;
    }
};

class ReturnStatement : public Statement {
public:
    // Null for a bare `return;`.
    ExpressionPtr expression;

    explicit ReturnStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return expression ? "return " + expression->toString() + ";" : "return;";
    }
};

// `throw <expr>;`, restricted to string operands (checked by DanglingChecker, not the grammar).
class ThrowStatement : public Statement {
public:
    ExpressionPtr expression;

    explicit ThrowStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return "throw " + expression->toString() + ";";
    }
};

class FunctionDeclaration : public Statement {
public:
    std::string returnType;
    std::string name;
    // (type, name) pairs, like MethodDeclaration. C++-style: `int f(int a, Node b)`.
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;
    // Enclosing namespace, or empty at top level. The builder flattens
    // `namespace X { int f() {...} }` into a top-level function with namespaceName "X".
    std::string namespaceName;

    FunctionDeclaration(const std::string& retType,
                        const std::string& func_name,
                        std::vector<std::pair<std::string, std::string>> params,
                        std::vector<StatementPtr> statements)
        : returnType(retType), name(func_name),
          parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = returnType + " " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// `ext_fn ReturnType name(params) [borrows N];` -- a signature for a function whose
// implementation is hand-written C++ outside CeeLing's analysis. It has no body, so
// the checkers have nothing to infer from; only the declared returnBorrowsParam counts.
class ExternFunctionDeclaration : public Statement {
public:
    std::string returnType;
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    // Index into `parameters` that the return value borrows from, or -1 if the result
    // is fresh (same meaning as FunctionSummary::returnBorrowsParam).
    int returnBorrowsParam = -1;
    // Indices of parameters declared `ref` (allowed only in ext_fn). A container ref
    // lowers to `const T&`, a primitive ref to `T&` (out-parameter).
    std::set<size_t> refParams;

    ExternFunctionDeclaration(const std::string& retType,
                              const std::string& func_name,
                              std::vector<std::pair<std::string, std::string>> params,
                              int borrowsParam)
        : returnType(retType), name(func_name),
          parameters(std::move(params)), returnBorrowsParam(borrowsParam) {}

    std::string toString() const override {
        std::string result = "ext_fn " + returnType + " " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            if (refParams.count(i)) result += "ref ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ")";
        if (returnBorrowsParam >= 0) {
            result += " borrows " + std::to_string(returnBorrowsParam);
        }
        result += ";";
        return result;
    }
};

class MainFunction : public Statement {
public:
    std::string returnType;
    std::vector<StatementPtr> body;

    MainFunction(const std::string& type, std::vector<StatementPtr> statements)
        : returnType(type), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = returnType + " main() {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class NewExpression : public Expression {
public:
    std::string className;
    std::vector<ExpressionPtr> arguments;

    NewExpression(const std::string& cls, std::vector<ExpressionPtr> args)
        : className(cls), arguments(std::move(args)) {}

    std::string toString() const override {
        std::string result = "new " + className + "(";
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i > 0) result += ", ";
            result += arguments[i]->toString();
        }
        result += ")";
        return result;
    }
};

// A `closure IFace [captures] (params) { body }` expression. Transient: the closure
// desugaring pass replaces each one with a NewExpression of a synthesized class
// (one field per capture, one method holding `body`) before any checker or the
// emitter runs.
class ClosureExpression : public Expression {
public:
    std::string interfaceName;
    std::vector<std::string> captures;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    ClosureExpression(const std::string& iface, std::vector<std::string> caps,
                      std::vector<std::pair<std::string, std::string>> params,
                      std::vector<StatementPtr> stmts)
        : interfaceName(iface), captures(std::move(caps)),
          parameters(std::move(params)), body(std::move(stmts)) {}

    std::string toString() const override {
        std::string result = "closure " + interfaceName + " [";
        for (size_t i = 0; i < captures.size(); ++i) {
            if (i > 0) result += ", ";
            result += captures[i];
        }
        result += "] (";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class MemberAccess : public Expression {
public:
    ExpressionPtr object;
    std::string member;

    MemberAccess(ExpressionPtr obj, const std::string& memberName)
        : object(std::move(obj)), member(memberName) {}

    std::string toString() const override {
        return object->toString() + "->" + member;
    }
};

// Subscript access `v[i]`; the index is an arbitrary expression.
class SubscriptExpression : public Expression {
public:
    ExpressionPtr object;
    ExpressionPtr index;

    SubscriptExpression(ExpressionPtr obj, ExpressionPtr idx)
        : object(std::move(obj)), index(std::move(idx)) {}

    std::string toString() const override {
        return object->toString() + "[" + index->toString() + "]";
    }
};

class IfStatement : public Statement {
public:
    ExpressionPtr condition;
    std::vector<StatementPtr> body;
    // For `else if`, a single nested IfStatement; for `else { ... }`, its statements.
    std::vector<StatementPtr> elseBody;

    IfStatement(ExpressionPtr cond, std::vector<StatementPtr> statements)
        : condition(std::move(cond)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "if (" + condition->toString() + ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        if (!elseBody.empty()) {
            result += " else {\n";
            for (const auto& stmt : elseBody) {
                result += "    " + stmt->toString() + "\n";
            }
            result += "}";
        }
        return result;
    }
};

// `break;` / `continue;` target the innermost loop (no labels).
class BreakStatement : public Statement {
public:
    std::string toString() const override { return "break;"; }
};

class ContinueStatement : public Statement {
public:
    std::string toString() const override { return "continue;"; }
};

class WhileStatement : public Statement {
public:
    ExpressionPtr condition;
    std::vector<StatementPtr> body;

    WhileStatement(ExpressionPtr cond, std::vector<StatementPtr> statements)
        : condition(std::move(cond)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "while (" + condition->toString() + ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// Range-based `for` only: `for (Type name : container) { ... }`.
class ForStatement : public Statement {
public:
    std::string loopVarType;
    std::string loopVarName;
    ExpressionPtr container;
    std::vector<StatementPtr> body;

    ForStatement(const std::string& varType, const std::string& varName,
                ExpressionPtr containerExpr, std::vector<StatementPtr> statements)
        : loopVarType(varType), loopVarName(varName),
          container(std::move(containerExpr)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "for (" + loopVarType + " " + loopVarName + " : " +
                             container->toString() + ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// `try { ... } catch (Type name) { ... }`, a single catch clause. DanglingChecker
// accepts only catchType `string`.
class TryStatement : public Statement {
public:
    std::vector<StatementPtr> body;
    std::string catchType;
    std::string catchIdentifier;
    std::vector<StatementPtr> catchBody;

    TryStatement(std::vector<StatementPtr> tryBody, const std::string& cType,
                const std::string& cIdentifier, std::vector<StatementPtr> cBody)
        : body(std::move(tryBody)), catchType(cType), catchIdentifier(cIdentifier),
          catchBody(std::move(cBody)) {}

    std::string toString() const override {
        std::string result = "try {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "} catch (" + catchType + " " + catchIdentifier + ") {\n";
        for (const auto& stmt : catchBody) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class IncludeStatement : public Statement {
public:
    std::string includeFile;

    explicit IncludeStatement(const std::string& file)
        : includeFile(file) {}

    std::string toString() const override {
        return "#include " + includeFile;
    }
};

class ClassDeclaration : public Statement {
public:
    std::string name;
    // Single base class (`class D : public B`), or empty. The grammar admits one base only.
    std::string baseClass;
    // Enclosing namespace, or empty. The builder flattens `namespace X { class C {...} }`
    // into a top-level class with namespaceName "X"; only AstEmitter reads it, to
    // re-wrap the class on output.
    std::string namespaceName;
    std::vector<StatementPtr> members;
    // True if declared with `struct`: a plain value type, stored bare and never
    // wrapped in a smart pointer (see SmartPointerAnalyzer::classifyValueTypeClasses).
    bool isValueType = false;
    // True for the class closure_desugar synthesizes for a `spawn(closure ...)` job.
    // Its object captures are moved in as unique owners (see DanglingChecker::checkSpawnCall).
    bool isSpawnJob = false;

    ClassDeclaration(const std::string& className, std::vector<StatementPtr> classMembers)
        : name(className), members(std::move(classMembers)) {}

    std::string toString() const override {
        std::string result = "class " + name;
        if (!baseClass.empty()) result += " : public " + baseClass;
        result += " {\npublic:\n";
        for (const auto& member : members) {
            result += "    " + member->toString() + "\n";
        }
        result += "};";
        return result;
    }
};

// An unscoped enum: `enum Name { A, B, C };`. Enumerators convert to `int` (there are
// no casts to convert a scoped enum). An enum value is a plain value, not tracked for
// moves or observers.
class EnumDeclaration : public Statement {
public:
    std::string name;
    std::vector<std::string> enumerators;
    // Enclosing namespace, or empty; same flattening as ClassDeclaration::namespaceName.
    std::string namespaceName;

    EnumDeclaration(const std::string& enumName, std::vector<std::string> values)
        : name(enumName), enumerators(std::move(values)) {}

    std::string toString() const override {
        std::string result = "enum " + name + " { ";
        for (size_t i = 0; i < enumerators.size(); ++i) {
            if (i > 0) result += ", ";
            result += enumerators[i];
        }
        result += " };";
        return result;
    }
};

class ConstructorDeclaration : public Statement {
public:
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    ConstructorDeclaration(const std::string& ctorName,
                          std::vector<std::pair<std::string, std::string>> params,
                          std::vector<StatementPtr> statements)
        : name(ctorName), parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ") {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class MethodDeclaration : public Statement {
public:
    std::string returnType;
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;
    bool isConst = false;      // trailing `const`
    bool isVirtual = false;    // `virtual` before the return type
    bool isOverride = false;   // `override` after the parameter list
    // A method is ordinary (both empty), a primary generic (`typeParameter`, "T" for
    // `<T> T as()`) or a specialization (`specializationArg`, "int" for `int as<int>()`).
    std::string typeParameter;
    std::string specializationArg;
    // True if parsed from a top-level `ReturnType ClassName::method<Type>(...) {...}`.
    // The builder splices it into the target class's `members`; the flag tells
    // AstEmitter to emit the definition at namespace scope, not inside the class body.
    bool isExternalSpecialization = false;

    MethodDeclaration(const std::string& retType,
                     const std::string& methodName,
                     std::vector<std::pair<std::string, std::string>> params,
                     std::vector<StatementPtr> statements)
        : returnType(retType), name(methodName), parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result;
        if (isVirtual) result += "virtual ";
        result += returnType + " " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ")";
        if (isConst) result += " const";
        if (isOverride) result += " override";
        result += " {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

// Emission-only placeholder for an external method specialization. The method itself
// lives in the target class's `members`, but the specialization may use a type declared
// after that class, so AstEmitter emits it at the specialization's original top-level
// position, marked here. The checkers skip this node.
class ExternalSpecializationMarker : public Statement {
public:
    std::string targetClassName;
    MethodDeclaration* method;  // non-owning; owned by targetClass->members

    ExternalSpecializationMarker(std::string targetClass, MethodDeclaration* m)
        : targetClassName(std::move(targetClass)), method(m) {}

    std::string toString() const override {
        return "external_specialization_marker(" + targetClassName + "::" + method->name + ")";
    }
};

class Program : public ASTNode {
public:
    std::vector<StatementPtr> statements;

    // External method specializations (target class name, method) whose class is not
    // in this Program yet; it may arrive by merging included files. The caller retries
    // with ASTBuilder::spliceExternalSpecializations after merging.
    std::vector<std::pair<std::string, StatementPtr>> unresolvedExternalSpecializations;

    void addStatement(StatementPtr stmt) {
        statements.push_back(std::move(stmt));
    }

    std::string toString() const override {
        std::string result;
        for (const auto& stmt : statements) {
            result += stmt->toString() + "\n";
        }
        return result;
    }
};

using ProgramPtr = std::unique_ptr<Program>;

} // namespace cling