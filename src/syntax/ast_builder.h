#pragma once
#include "syntax/ast.h"
#include "syntax/ts_language.h"
#include <memory>
#include <vector>

namespace cling {

class ASTBuilder {
public:
    explicit ASTBuilder();
    ~ASTBuilder() { ts_parser_delete(parser); }
    ASTBuilder(const ASTBuilder&) = delete;
    ASTBuilder& operator=(const ASTBuilder&) = delete;
    ProgramPtr buildProgram(const std::string& source);

    // Splices every entry of program.unresolvedExternalSpecializations into its target
    // class. buildProgram calls it once; the caller must call it again after merging
    // included files, which can bring in the target class. Throws
    // error[semantic/unknown-specialization-target] for an entry still unresolved.
    static void spliceExternalSpecializations(Program& program);

private:
    TSParser* parser;

    // Every base clause names a declared class and the inheritance graph is acyclic.
    void validateInheritance(const Program& program);

    StatementPtr buildStatement(TSNode node, const std::string& source);
    StatementPtr buildVariableDeclaration(TSNode node, const std::string& source);
    StatementPtr buildContainerDeclaration(TSNode node, const std::string& source);
    StatementPtr buildMemberVariableDeclaration(TSNode node, const std::string& source);
    StatementPtr buildAssignmentStatement(TSNode node, const std::string& source);
    StatementPtr buildExpressionStatement(TSNode node, const std::string& source);
    StatementPtr buildCommentStatement(TSNode node, const std::string& source);
    StatementPtr buildIncludeStatement(TSNode node, const std::string& source);
    StatementPtr buildFunctionDeclaration(TSNode node, const std::string& source);
    StatementPtr buildExternFunctionDeclaration(TSNode node, const std::string& source);
    StatementPtr buildClassDeclaration(TSNode node, const std::string& source);
    StatementPtr buildEnumDeclaration(TSNode node, const std::string& source);
    // Flattens a namespace body into top-level statements. Called from buildProgram
    // only (top-level namespaces), not from buildStatement.
    std::vector<StatementPtr> buildNamespaceDeclaration(TSNode node, const std::string& source);
    StatementPtr buildConstructorDeclaration(TSNode node, const std::string& source);
    StatementPtr buildMethodDeclaration(TSNode node, const std::string& source);
    // Called from buildProgram only: the result is spliced into an existing class.
    // `targetClass` receives the class name from the `ClassName::` qualifier.
    StatementPtr buildExternalMethodSpecialization(TSNode node, const std::string& source,
                                                    std::string& targetClass);
    StatementPtr buildMainFunction(TSNode node, const std::string& source);
    StatementPtr buildReturnStatement(TSNode node, const std::string& source);
    StatementPtr buildIfStatement(TSNode node, const std::string& source);
    StatementPtr buildWhileStatement(TSNode node, const std::string& source);
    StatementPtr buildForStatement(TSNode node, const std::string& source);
    StatementPtr buildThrowStatement(TSNode node, const std::string& source);
    StatementPtr buildTryStatement(TSNode node, const std::string& source);
    ExpressionPtr buildExpression(TSNode node, const std::string& source);
    ExpressionPtr buildIdentifier(TSNode node, const std::string& source);
    ExpressionPtr buildScopedIdentifier(TSNode node, const std::string& source);
    ExpressionPtr buildStringLiteral(TSNode node, const std::string& source);
    ExpressionPtr buildCharLiteral(TSNode node, const std::string& source);
    ExpressionPtr buildNumber(TSNode node, const std::string& source);
    ExpressionPtr buildFunctionCall(TSNode node, const std::string& source);
    ExpressionPtr buildTemplateCall(TSNode node, const std::string& source);
    ExpressionPtr buildNewExpression(TSNode node, const std::string& source);
    ExpressionPtr buildClosureExpression(TSNode node, const std::string& source);
    ExpressionPtr buildBinaryExpression(TSNode node, const std::string& source);
    ExpressionPtr buildUnaryExpression(TSNode node, const std::string& source);
    ExpressionPtr buildMemberAccess(TSNode node, const std::string& source);
    ExpressionPtr buildSubscriptExpression(TSNode node, const std::string& source);

    std::string getNodeText(TSNode node, const std::string& source);
};

using ASTBuilderPtr = std::unique_ptr<ASTBuilder>;

} // namespace cling