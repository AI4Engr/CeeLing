#pragma once

#include "../cst_to_ast/ast.h"
#include "smart_pointer_analyzer.h"
#include <string>
#include <sstream>
#include <set>

namespace cling {

class CppGenerator {
public:
    std::string generateCpp(const Program& program, const std::string& source);

private:
    std::stringstream output;
    std::set<std::string> classNames;  // Store class names for transformation
    SmartPointerAnalyzer analyzer;     // Two-pass smart pointer analyzer

    void generateStatement(const Statement& stmt);
    void generateVariableDeclaration(const VariableDeclaration& decl);
    void generateAssignmentStatement(const AssignmentStatement& stmt);
    void generatePrintStatement(const PrintStatement& stmt);
    void generateCommentStatement(const CommentStatement& stmt);
    void generateFunctionDeclaration(const FunctionDeclaration& func);
    void generateMethodDeclaration(const MethodDeclaration& method);
    void generateMainFunction(const MainFunction& func);
    void generateClassDeclaration(const ClassDeclaration& cls);
    void generateConstructorDeclaration(const ConstructorDeclaration& ctor);
    void generateIncludeStatement(const IncludeStatement& include);
    void generateReturnStatement(const ReturnStatement& stmt);
    void generateIfStatement(const IfStatement& stmt);

    std::string generateExpression(const Expression& expr);
    std::string generateBinaryExpression(const BinaryExpression& expr);
    std::string generateFunctionCall(const FunctionCall& call);
    // targetVar / targetClass enable smart-pointer-aware factory selection
    std::string generateNewExpression(const NewExpression& newExpr,
                                      const std::string& targetVar = "",
                                      const std::string& targetClass = "");
    std::string generateMemberAccess(const MemberAccess& access);
    std::string generateIdentifier(const Identifier& id);
    std::string generateNumber(const Number& num);

    // Helper functions
    bool isClassName(const std::string& type) const;
    std::string getSmartPointerType(const std::string& className, const std::string& variableName) const;
    // Build "std::make_unique/shared<C>(args...)" string
    std::string makeFactory(const std::string& ptrType, const std::string& className,
                            const std::vector<ExpressionPtr>& args);
};

} // namespace cling