#pragma once
#include <memory>
#include <string>
#include <vector>
#include <variant>

// AST Node definitions for CLing language

namespace cling {

// Source position of a node, 1-based line/column. line == 0 means "unknown"
// (e.g. synthesized nodes). Populated by the AST builder from the Tree-sitter CST;
// used by the safety analysis to point diagnostics at the originating .clg line.
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

// A qualified name like `std::cout`, `std::endl`, `std::vector`. Stored as the full
// joined text; the generator emits it verbatim.
class ScopedIdentifier : public Expression {
public:
    std::string qualifiedName;

    explicit ScopedIdentifier(const std::string& name) : qualifiedName(name) {}
    std::string toString() const override { return qualifiedName; }
};

// A double-quoted string literal, e.g. `" "` in `std::cout << " ";`. Holds the
// original text including the quotes; emitted verbatim.
class StringLiteral : public Expression {
public:
    std::string original_text;

    explicit StringLiteral(const std::string& text) : original_text(text) {}
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
    // For method calls `recv.method(args)`, `callee` holds the `recv.method`
    // MemberAccess so the receiver's access operator can be lowered correctly.
    // Null for a plain call `f(args)` (use function_name).
    ExpressionPtr callee;

    FunctionCall(const std::string& name, std::vector<ExpressionPtr> args)
        : function_name(name), arguments(std::move(args)) {}

    std::string toString() const override {
        std::string result = function_name + "(";
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

// A container declared without an initializer, e.g. `std::vector<int> v;` --
// default-constructs to empty. The one exception to CeeLing's definite-
// initialization rule (see CLAUDE.md hatch #18): a container's default (empty)
// state is always well-defined and safe to read, unlike a bare scalar or class
// declaration. `type` is always a generic-type string (e.g. "std::vector<int>");
// the grammar restricts this production to $.generic_type, never a plain type.
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
    // For a bare-name target (`a = ...`), `variable` holds the name and `target` is
    // null. For a member target (`a.b = ...`), `target` holds the MemberAccess and
    // `variable` holds the root object name (`a`) for the analysis passes.
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

// A bare expression used as a statement, e.g. `sink(a);`. Its main role for the
// safety analysis is to make move events at call sites visible to the checker.
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
    ExpressionPtr expression;

    explicit ReturnStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return "return " + expression->toString() + ";";
    }
};

class FunctionDeclaration : public Statement {
public:
    std::string returnType;
    std::string name;
    // (type, name) pairs, like MethodDeclaration. C++-style: `int f(int a, Node b)`.
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

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

class IfStatement : public Statement {
public:
    ExpressionPtr condition;
    std::vector<StatementPtr> body;
    // `else` branch. For `else if`, this holds a single nested IfStatement; for a
    // plain `else { ... }`, it holds that block's statements. Empty if no else.
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

// Range-based `for` only: `for (Type name : container) { ... }`. Matches native
// C++ range-for 1:1, so the emitter needs no translation logic. `loopVarType`/
// `loopVarName` are the per-iteration binding; `container` is the collection
// expression being iterated.
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
    std::vector<StatementPtr> members;

    ClassDeclaration(const std::string& className, std::vector<StatementPtr> classMembers)
        : name(className), members(std::move(classMembers)) {}

    std::string toString() const override {
        std::string result = "class " + name + " {\npublic:\n";
        for (const auto& member : members) {
            result += "    " + member->toString() + "\n";
        }
        result += "};";
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
    bool isConst = false;  // true if declared with a trailing `const` qualifier

    MethodDeclaration(const std::string& retType,
                     const std::string& methodName,
                     std::vector<std::pair<std::string, std::string>> params,
                     std::vector<StatementPtr> statements)
        : returnType(retType), name(methodName), parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = returnType + " " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i].first + " " + parameters[i].second;
        }
        result += ")";
        if (isConst) result += " const";
        result += " {\n";
        for (const auto& stmt : body) {
            result += "    " + stmt->toString() + "\n";
        }
        result += "}";
        return result;
    }
};

class Program : public ASTNode {
public:
    std::vector<StatementPtr> statements;

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