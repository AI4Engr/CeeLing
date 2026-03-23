#include "cpp_generator.h"
#include <iostream>
#include <stdexcept>

namespace cling {

// ─── helpers ───────────────────────────────────────────────────────────────

std::string CppGenerator::makeFactory(const std::string& ptrType,
                                       const std::string& className,
                                       const std::vector<ExpressionPtr>& args) {
    std::string factoryName = (ptrType == "std::unique_ptr") ? "std::make_unique" : "std::make_shared";
    std::string result = factoryName + "<" + className + ">(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) result += ", ";
        result += generateExpression(*args[i]);
    }
    result += ")";
    return result;
}

// ─── main entry point ──────────────────────────────────────────────────────

std::string CppGenerator::generateCpp(const Program& program, const std::string& /*source*/) {
    output.str("");
    output.clear();

    // Two-pass smart pointer analysis
    std::cerr << "=== PERFORMING TWO-PASS SMART POINTER ANALYSIS ===" << std::endl;
    analyzer.analyzeProgram(program);

    // Collect all class names
    classNames.clear();
    for (const auto& stmt : program.statements) {
        if (auto classDecl = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames.insert(classDecl->name);
        }
    }

    // Prepend #include <memory> / <utility> if classes are present and not already included
    if (!classNames.empty()) {
        bool hasMemory  = false;
        bool hasUtility = false;
        for (const auto& stmt : program.statements) {
            if (auto inc = dynamic_cast<const IncludeStatement*>(stmt.get())) {
                if (inc->includeFile.find("<memory>")  != std::string::npos) hasMemory  = true;
                if (inc->includeFile.find("<utility>") != std::string::npos) hasUtility = true;
            }
        }
        if (!hasMemory)  output << "#include <memory>\n";
        if (!hasUtility) output << "#include <utility>\n";
    }

    // Walk AST and generate C++
    for (const auto& stmt : program.statements) {
        generateStatement(*stmt);
    }

    return output.str();
}

// ─── statement dispatch ────────────────────────────────────────────────────

void CppGenerator::generateStatement(const Statement& stmt) {
    if (auto varDecl = dynamic_cast<const VariableDeclaration*>(&stmt)) {
        generateVariableDeclaration(*varDecl);
    } else if (auto assignment = dynamic_cast<const AssignmentStatement*>(&stmt)) {
        generateAssignmentStatement(*assignment);
    } else if (auto printStmt = dynamic_cast<const PrintStatement*>(&stmt)) {
        generatePrintStatement(*printStmt);
    } else if (auto funcDecl = dynamic_cast<const FunctionDeclaration*>(&stmt)) {
        generateFunctionDeclaration(*funcDecl);
    } else if (auto methodDecl = dynamic_cast<const MethodDeclaration*>(&stmt)) {
        generateMethodDeclaration(*methodDecl);
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
    } else if (auto ifStmt = dynamic_cast<const IfStatement*>(&stmt)) {
        generateIfStatement(*ifStmt);
    }
}

// ─── statement generators ──────────────────────────────────────────────────

void CppGenerator::generateVariableDeclaration(const VariableDeclaration& decl) {
    if (!decl.expression) {
        // Member variable without initializer
        if (isClassName(decl.type)) {
            std::string ptrType = getSmartPointerType(decl.type, decl.identifier);
            output << ptrType << "<" << decl.type << "> " << decl.identifier << ";\n";
        } else {
            output << decl.type << " " << decl.identifier << ";\n";
        }
        return;
    }

    if (isClassName(decl.type)) {
        std::string ptrType = getSmartPointerType(decl.type, decl.identifier);
        if (auto newExpr = dynamic_cast<const NewExpression*>(decl.expression.get())) {
            // Use pointer-type-aware factory: make_unique or make_shared
            output << ptrType << "<" << decl.type << "> " << decl.identifier
                   << " = " << makeFactory(ptrType, newExpr->className, newExpr->arguments) << ";\n";
        } else {
            // Assigned from another variable — shared ownership
            output << "std::shared_ptr<" << decl.type << "> " << decl.identifier
                   << " = " << generateExpression(*decl.expression) << ";\n";
        }
        return;
    }

    output << decl.type << " " << decl.identifier
           << " = " << generateExpression(*decl.expression) << ";\n";
}

void CppGenerator::generateAssignmentStatement(const AssignmentStatement& stmt) {
    if (auto newExpr = dynamic_cast<const NewExpression*>(stmt.expression.get())) {
        // Try to determine smart pointer type from the target variable name
        std::string ptrType = getSmartPointerType(newExpr->className, stmt.variable);
        output << stmt.variable << " = " << makeFactory(ptrType, newExpr->className, newExpr->arguments) << ";\n";
    } else {
        output << stmt.variable << " = " << generateExpression(*stmt.expression) << ";\n";
    }
}

void CppGenerator::generatePrintStatement(const PrintStatement& stmt) {
    output << "std::cout << " << generateExpression(*stmt.expression) << " << std::endl;\n";
}

void CppGenerator::generateCommentStatement(const CommentStatement& stmt) {
    output << "//" << stmt.content << "\n";
}

// Fix 3: actually generate C++ for standalone functions.
// CeeLing function parameters have no type annotations, so we use 'auto'
// (abbreviated function template, valid in C++20; also accepted by GCC/Clang in C++14 mode).
void CppGenerator::generateFunctionDeclaration(const FunctionDeclaration& func) {
    output << "auto " << func.name << "(";
    for (size_t i = 0; i < func.parameters.size(); ++i) {
        if (i > 0) output << ", ";
        output << "auto " << func.parameters[i];
    }
    output << ") {\n";
    for (const auto& stmt : func.body) {
        output << "    ";
        generateStatement(*stmt);
    }
    output << "}\n";
}

void CppGenerator::generateMethodDeclaration(const MethodDeclaration& method) {
    output << method.returnType << " " << method.name << "(";
    for (size_t i = 0; i < method.parameters.size(); ++i) {
        if (i > 0) output << ", ";
        output << method.parameters[i].first << " " << method.parameters[i].second;
    }
    output << ") {\n";
    for (const auto& stmt : method.body) {
        output << "    ";
        generateStatement(*stmt);
    }
    output << "}\n";
}

void CppGenerator::generateReturnStatement(const ReturnStatement& stmt) {
    output << "return " << generateExpression(*stmt.expression) << ";\n";
}

void CppGenerator::generateIfStatement(const IfStatement& stmt) {
    output << "if (" << generateExpression(*stmt.condition) << ") {\n";
    for (const auto& bodyStmt : stmt.body) {
        output << "    ";
        generateStatement(*bodyStmt);
    }
    output << "}\n";
}

void CppGenerator::generateMainFunction(const MainFunction& func) {
    output << func.returnType << " main() {\n";
    for (const auto& stmt : func.body) {
        output << "    ";
        generateStatement(*stmt);
    }
    output << "}\n";
}

void CppGenerator::generateClassDeclaration(const ClassDeclaration& cls) {
    output << "class " << cls.name << " {\npublic:\n";
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
        output << "    ";
        generateStatement(*stmt);
    }
    output << "}\n";
}

void CppGenerator::generateIncludeStatement(const IncludeStatement& include) {
    output << include.includeFile << "\n";
}

// ─── expression generators ─────────────────────────────────────────────────

std::string CppGenerator::generateExpression(const Expression& expr) {
    if (auto binExpr = dynamic_cast<const BinaryExpression*>(&expr)) {
        return generateBinaryExpression(*binExpr);
    } else if (auto funcCall = dynamic_cast<const FunctionCall*>(&expr)) {
        return generateFunctionCall(*funcCall);
    } else if (auto newExpr = dynamic_cast<const NewExpression*>(&expr)) {
        return generateNewExpression(*newExpr);
    } else if (auto memberAccess = dynamic_cast<const MemberAccess*>(&expr)) {
        return generateMemberAccess(*memberAccess);
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
    for (size_t i = 0; i < call.arguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += generateExpression(*call.arguments[i]);
    }
    result += ")";
    return result;
}

// When called from expression context (no variable assignment target),
// default to make_shared as the safe conservative choice.
std::string CppGenerator::generateNewExpression(const NewExpression& newExpr,
                                                  const std::string& targetVar,
                                                  const std::string& targetClass) {
    std::string ptrType = targetVar.empty()
        ? "std::shared_ptr"
        : getSmartPointerType(targetClass.empty() ? newExpr.className : targetClass, targetVar);
    return makeFactory(ptrType, newExpr.className, newExpr.arguments);
}

std::string CppGenerator::generateMemberAccess(const MemberAccess& access) {
    return generateExpression(*access.object) + "->" + access.member;
}

std::string CppGenerator::generateIdentifier(const Identifier& id) {
    return id.name;
}

std::string CppGenerator::generateNumber(const Number& num) {
    return num.original_text;
}

// ─── helpers ───────────────────────────────────────────────────────────────

bool CppGenerator::isClassName(const std::string& type) const {
    return classNames.find(type) != classNames.end();
}

std::string CppGenerator::getSmartPointerType(const std::string& className,
                                               const std::string& variableName) const {
    PointerType type = analyzer.getPointerType(className, variableName);
    switch (type) {
        case PointerType::UNIQUE: return "std::unique_ptr";
        case PointerType::WEAK:   return "std::weak_ptr";
        default:                  return "std::shared_ptr";
    }
}

} // namespace cling
