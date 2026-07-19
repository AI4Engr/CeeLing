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

class FunctionCall : public Expression {
public:
    std::string function_name;
    std::vector<ExpressionPtr> arguments;

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

    VariableDeclaration(const std::string& t, const std::string& id, ExpressionPtr expr)
        : type(t), identifier(id), expression(std::move(expr)) {}

    std::string toString() const override {
        return type + " " + identifier + " = " + expression->toString() + ";";
    }
};

class AssignmentStatement : public Statement {
public:
    std::string variable;
    ExpressionPtr expression;

    AssignmentStatement(const std::string& var, ExpressionPtr expr)
        : variable(var), expression(std::move(expr)) {}

    std::string toString() const override {
        return variable + " := " + expression->toString() + ";";
    }
};

class PrintStatement : public Statement {
public:
    ExpressionPtr expression;

    explicit PrintStatement(ExpressionPtr expr)
        : expression(std::move(expr)) {}

    std::string toString() const override {
        return "print(" + expression->toString() + ");";
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
    std::string name;
    std::vector<std::string> parameters;
    std::vector<StatementPtr> body;

    FunctionDeclaration(const std::string& func_name,
                       std::vector<std::string> params,
                       std::vector<StatementPtr> statements)
        : name(func_name), parameters(std::move(params)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "fn " + name + "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            if (i > 0) result += ", ";
            result += parameters[i];
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

    IfStatement(ExpressionPtr cond, std::vector<StatementPtr> statements)
        : condition(std::move(cond)), body(std::move(statements)) {}

    std::string toString() const override {
        std::string result = "if (" + condition->toString() + ") {\n";
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
        result += ") {\n";
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