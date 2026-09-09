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

    // Re-attempts splicing every entry in program.unresolvedExternalSpecializations
    // into its target class's `members` (see Program::unresolvedExternalSpecializations'
    // comment). buildProgram calls this once internally for the common same-file
    // case; main.cpp must call it AGAIN after mergeExternalDeclarations, since only
    // that later step can bring in a target class declared in a different .clg file
    // reached via #include. Throws error[semantic/unknown-specialization-target] for
    // any entry still unresolved after this call (the caller is expected to call this
    // only once merging is fully done, so "still not found" here means genuinely
    // unknown, not "not merged in yet"). No-op if the list is already empty.
    static void spliceExternalSpecializations(Program& program);

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
    // Not reachable from buildStatement's own dispatch -- called directly from
    // buildProgram (mirrors buildNamespaceDeclaration's own reasoning: the
    // result must be spliced into an EXISTING class's members elsewhere,
    // which doesn't fit buildStatement's one-node-in/one-statement-out
    // contract). `targetClass` is an out-parameter: the class name parsed
    // from the `ClassName::` qualifier.
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