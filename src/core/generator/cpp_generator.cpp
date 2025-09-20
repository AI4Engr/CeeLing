#include "cpp_generator.h"
#include <iostream>
#include <regex>

namespace cling {

std::string CppGenerator::generateCpp(const Program& program, const std::string& source) {
    output.str("");
    output.clear();

    // First pass: collect all class names
    classNames.clear();
    for (const auto& stmt : program.statements) {
        if (auto classDecl = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames.insert(classDecl->name);
        }
    }

    // For source preservation, we'll generate by traversing the source and replacing parts
    std::string result = source;

    // Transform class member variable declarations with smart pointer versions
    for (const std::string& className : classNames) {
        // Pattern: "ClassName variableName;"
        std::regex pattern("\\b" + className + "\\s+(\\w+);");
        std::string replacement = "std::shared_ptr<" + className + "> $1;";
        result = std::regex_replace(result, pattern, replacement);
    }

    // Transform class variable declarations with assignment (not new)
    for (const std::string& className : classNames) {
        // Pattern: "ClassName variable = someVariable;" (but not "new ClassName")
        std::regex pattern("\\b" + className + "\\s+(\\w+)\\s*=\\s*(?!new\\s)([^;]+);");
        std::string replacement = "std::shared_ptr<" + className + "> $1 = $2;";
        result = std::regex_replace(result, pattern, replacement);
    }

    // Transform new expressions to make_shared
    for (const std::string& className : classNames) {
        // Pattern: "ClassName var = new ClassName(...)"
        std::regex pattern("\\b" + className + "\\s+(\\w+)\\s*=\\s*new\\s+" + className + "\\s*\\(([^)]*)\\)");
        std::string replacement = "std::shared_ptr<" + className + "> $1 = std::make_shared<" + className + ">($2)";
        result = std::regex_replace(result, pattern, replacement);
    }

    // Transform assignment of new expressions to make_shared
    for (const std::string& className : classNames) {
        // Pattern: "variableName = new ClassName(...)"
        std::regex pattern("(\\w+)\\s*=\\s*new\\s+" + className + "\\s*\\(([^)]*)\\)");
        std::string replacement = "$1 = std::make_shared<" + className + ">($2)";
        result = std::regex_replace(result, pattern, replacement);
    }

    // Transform member access from . to ->
    for (const std::string& className : classNames) {
        // Pattern: "variableName.memberName" -> "variableName->memberName"
        // This is a simplified transformation - in practice you'd want more sophisticated handling
        std::regex pattern("(\\w+)\\.(\\w+)");
        std::string replacement = "$1->$2";
        result = std::regex_replace(result, pattern, replacement);
    }

    return result;
}

void CppGenerator::generateStatement(const Statement& stmt) {
    if (auto varDecl = dynamic_cast<const VariableDeclaration*>(&stmt)) {
        generateVariableDeclaration(*varDecl);
    } else if (auto assignment = dynamic_cast<const AssignmentStatement*>(&stmt)) {
        generateAssignmentStatement(*assignment);
    } else if (auto printStmt = dynamic_cast<const PrintStatement*>(&stmt)) {
        generatePrintStatement(*printStmt);
    } else if (auto funcDecl = dynamic_cast<const FunctionDeclaration*>(&stmt)) {
        generateFunctionDeclaration(*funcDecl);
    } else if (auto mainFunc = dynamic_cast<const MainFunction*>(&stmt)) {
        generateMainFunction(*mainFunc);
    } else if (auto classDecl = dynamic_cast<const ClassDeclaration*>(&stmt)) {
        generateClassDeclaration(*classDecl);
    } else if (auto ctorDecl = dynamic_cast<const ConstructorDeclaration*>(&stmt)) {
        generateConstructorDeclaration(*ctorDecl);
    } else if (auto includeStmt = dynamic_cast<const IncludeStatement*>(&stmt)) {
        generateIncludeStatement(*includeStmt);
    } else if (auto returnStmt = dynamic_cast<const ReturnStatement*>(&stmt)) {
        generateReturnStatement(*returnStmt);
    } else if (auto commentStmt = dynamic_cast<const CommentStatement*>(&stmt)) {
        generateCommentStatement(*commentStmt);
    }
}

void CppGenerator::generateVariableDeclaration(const VariableDeclaration& decl) {
    // Handle member variables without initialization (null expression)
    if (!decl.expression) {
        // For member variables without initialization, transform class types
        if (isClassName(decl.type)) {
            output << "std::shared_ptr<" << decl.type << "> " << decl.identifier << ";\n";
        } else {
            output << decl.type << " " << decl.identifier << ";\n";
        }
        return;
    }

    // Check if the type is a class name and the expression is a new expression
    if (isClassName(decl.type)) {
        if (auto newExpr = dynamic_cast<const NewExpression*>(decl.expression.get())) {
            // Transform "ClassName var = new ClassName(...)" to "std::shared_ptr<ClassName> var = std::make_shared<ClassName>(...)"
            output << "std::shared_ptr<" << decl.type << "> " << decl.identifier
                   << " = " << generateExpression(*decl.expression) << ";\n";
            return;
        }
    }

    // Normal variable declaration
    output << decl.type << " " << decl.identifier
           << " = " << generateExpression(*decl.expression) << ";\n";
}

void CppGenerator::generateAssignmentStatement(const AssignmentStatement& stmt) {
    output << stmt.variable
           << " = " << generateExpression(*stmt.expression) << ";\n";
}

void CppGenerator::generatePrintStatement(const PrintStatement& stmt) {
    output << "std::cout << " << generateExpression(*stmt.expression) << " << std::endl;\n";
}

void CppGenerator::generateCommentStatement(const CommentStatement& stmt) {
    output << "//" << stmt.content << "\n";
}

void CppGenerator::generateFunctionDeclaration(const FunctionDeclaration& func) {
    // Functions are generated before main - this is a simplified approach
    // In a more complete implementation, we'd do a two-pass generation
    std::cerr << "Warning: Function declarations not yet fully supported in C++ generation\n";
}

void CppGenerator::generateReturnStatement(const ReturnStatement& stmt) {
    output << "return " << generateExpression(*stmt.expression) << ";\n";
}

std::string CppGenerator::generateExpression(const Expression& expr) {
    if (auto binExpr = dynamic_cast<const BinaryExpression*>(&expr)) {
        return generateBinaryExpression(*binExpr);
    } else if (auto funcCall = dynamic_cast<const FunctionCall*>(&expr)) {
        return generateFunctionCall(*funcCall);
    } else if (auto newExpr = dynamic_cast<const NewExpression*>(&expr)) {
        return generateNewExpression(*newExpr);
    } else if (auto identifier = dynamic_cast<const Identifier*>(&expr)) {
        return generateIdentifier(*identifier);
    } else if (auto number = dynamic_cast<const Number*>(&expr)) {
        return generateNumber(*number);
    }
    return "/* unknown expression */";
}

std::string CppGenerator::generateBinaryExpression(const BinaryExpression& expr) {
    return "(" + generateExpression(*expr.left) + " " +
           expr.operator_str + " " + generateExpression(*expr.right) + ")";
}

std::string CppGenerator::generateFunctionCall(const FunctionCall& call) {
    std::string result = call.function_name + "(";
    const auto& args = call.arguments;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) result += ", ";
        result += generateExpression(*args[i]);
    }
    result += ")";
    return result;
}

std::string CppGenerator::generateIdentifier(const Identifier& id) {
    return id.name;
}

std::string CppGenerator::generateNumber(const Number& num) {
    return num.original_text;
}

void CppGenerator::generateMainFunction(const MainFunction& func) {
    output << func.returnType << " main() {\n";
    for (const auto& stmt : func.body) {
        output << "    ";
        generateStatement(*stmt);
    }
    output << "}\n";
}

std::string CppGenerator::generateNewExpression(const NewExpression& newExpr) {
    // Transform "new ClassName(...)" to "std::make_shared<ClassName>(...)"
    std::string result = "std::make_shared<" + newExpr.className + ">(";
    for (size_t i = 0; i < newExpr.arguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += generateExpression(*newExpr.arguments[i]);
    }
    result += ")";
    return result;
}

bool CppGenerator::isClassName(const std::string& type) const {
    return classNames.find(type) != classNames.end();
}

void CppGenerator::generateClassDeclaration(const ClassDeclaration& cls) {
    output << "class " << cls.name << " {\n";
    output << "public:\n";

    for (const auto& member : cls.members) {
        output << "    ";
        generateStatement(*member);
    }

    output << "};\n";
}

void CppGenerator::generateConstructorDeclaration(const ConstructorDeclaration& ctor) {
    output << ctor.name << "(";

    for (size_t i = 0; i < ctor.parameters.size(); ++i) {
        if (i > 0) output << ", ";
        output << ctor.parameters[i].first << " " << ctor.parameters[i].second;
    }

    output << ") {\n";

    for (const auto& stmt : ctor.body) {
        output << "        ";
        generateStatement(*stmt);
    }

    output << "    }\n";
}

void CppGenerator::generateIncludeStatement(const IncludeStatement& include) {
    output << include.includeFile << "\n";
}

} // namespace cling