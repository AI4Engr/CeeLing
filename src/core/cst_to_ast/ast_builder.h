#pragma once
#include "ast.h"
#include "../tree_sitter_cling.h"
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

private:
    TSParser* parser;

    // Post-parse whole-program check: every base clause names a declared class, and
    // the inheritance graph is acyclic (a prerequisite for the base-before-derived
    // member flattening every analysis module does at collection time).
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
    StatementPtr buildClassDeclaration(TSNode node, const std::string& source);
    StatementPtr buildEnumDeclaration(TSNode node, const std::string& source);
    // Desugars a namespace body into a flat list of top-level statements (see the
    // .cpp for why this can't be an ordinary one-in-one-out buildStatement case).
    // Not reachable from buildStatement's own dispatch -- called directly from
    // buildProgram, since only top-level namespace declarations are supported.
    std::vector<StatementPtr> buildNamespaceDeclaration(TSNode node, const std::string& source);
    StatementPtr buildConstructorDeclaration(TSNode node, const std::string& source);
    StatementPtr buildMethodDeclaration(TSNode node, const std::string& source);
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
    ExpressionPtr buildBinaryExpression(TSNode node, const std::string& source);
    ExpressionPtr buildUnaryExpression(TSNode node, const std::string& source);
    ExpressionPtr buildMemberAccess(TSNode node, const std::string& source);
    ExpressionPtr buildSubscriptExpression(TSNode node, const std::string& source);

    std::string getNodeText(TSNode node, const std::string& source);
};

using ASTBuilderPtr = std::unique_ptr<ASTBuilder>;

} // namespace cling