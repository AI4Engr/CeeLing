#include "ast_builder.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>

namespace cling {

namespace {

std::string makeNodeError(const char* prefix, TSNode node, const std::string& source) {
    const TSPoint point = ts_node_start_point(node);
    const uint32_t startByte = ts_node_start_byte(node);
    const uint32_t endByte = ts_node_end_byte(node);
    const uint32_t clampedEnd = std::min<uint32_t>(endByte, static_cast<uint32_t>(source.size()));
    const uint32_t clampedStart = std::min<uint32_t>(startByte, clampedEnd);

    std::string snippet = source.substr(clampedStart, clampedEnd - clampedStart);
    if (snippet.size() > 120) {
        snippet = snippet.substr(0, 120) + "...";
    }

    std::ostringstream oss;
    oss << prefix
        << " at line " << (point.row + 1)
        << ", column " << (point.column + 1)
        << ", node type '" << ts_node_type(node) << "'";
    if (!snippet.empty()) {
        oss << ", near: " << snippet;
    }

    if (snippet.find(":=") != std::string::npos) {
        oss << ". Suggestion: use '=' for assignment.";
    } else if (!snippet.empty() && snippet.find(';') == std::string::npos &&
               snippet.find('{') == std::string::npos && snippet.find('}') == std::string::npos) {
        oss << ". Suggestion: check whether the statement is missing a trailing ';'.";
    } else if (snippet.find("->") != std::string::npos || snippet.find('.') != std::string::npos) {
        oss << ". Suggestion: check member-access syntax and make sure the full statement is supported.";
    }
    return oss.str();
}

} // namespace

ASTBuilder::ASTBuilder() {
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_cling());
}

std::string ASTBuilder::getNodeText(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    return source.substr(start, end - start);
}

ProgramPtr ASTBuilder::buildProgram(const std::string& source) {
    auto program = std::make_unique<Program>();

    TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(), source.length());
    if (!tree) {
        throw std::runtime_error("Failed to parse source with Tree-sitter");
    }

    TSNode root_node = ts_tree_root_node(tree);
    bool hasStructuralAnchor = false;
    bool hasUnresolvedTopLevel = false;
    TSNode firstUnresolvedNode{};

    uint32_t child_count = ts_node_child_count(root_node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        if (strcmp(ts_node_type(child), "ERROR") == 0 || ts_node_is_missing(child)) {
            if (!hasUnresolvedTopLevel) {
                firstUnresolvedNode = child;
            }
            hasUnresolvedTopLevel = true;
            continue;
        }

        auto stmt = buildStatement(child, source);
        if (stmt) {
            if (dynamic_cast<ClassDeclaration*>(stmt.get()) || dynamic_cast<MainFunction*>(stmt.get())) {
                hasStructuralAnchor = true;
            }
            program->addStatement(std::move(stmt));
        } else {
            if (!hasUnresolvedTopLevel) {
                firstUnresolvedNode = child;
            }
            hasUnresolvedTopLevel = true;
        }
    }

    if (program->statements.empty()) {
        std::string err = ts_node_is_null(firstUnresolvedNode)
            ? "Parse error: could not build any top-level statements"
            : makeNodeError("Parse error", firstUnresolvedNode, source);
        ts_tree_delete(tree);
        throw std::runtime_error(err);
    }

    if (hasUnresolvedTopLevel && !hasStructuralAnchor) {
        std::string err = ts_node_is_null(firstUnresolvedNode)
            ? "Parse error: unsupported top-level syntax"
            : makeNodeError("Parse error", firstUnresolvedNode, source);
        ts_tree_delete(tree);
        throw std::runtime_error(err);
    }

    ts_tree_delete(tree);
    return program;
}

// Stamp a node's source position from its originating CST node (1-based).
static void stampLoc(ASTNode* astNode, TSNode tsNode) {
    if (astNode == nullptr) return;
    const TSPoint p = ts_node_start_point(tsNode);
    astNode->loc.line = static_cast<int>(p.row) + 1;
    astNode->loc.column = static_cast<int>(p.column) + 1;
}

// Observers (`ref`) are local-only: they may not appear as function parameters.
// A `ref` in a parameter list is a hard error (the language subset forbids passing
// observers into functions — arguments always move ownership).
static void rejectObserverParams(TSNode paramList) {
    const uint32_t n = ts_node_child_count(paramList);
    for (uint32_t j = 0; j < n; ++j) {
        TSNode child = ts_node_child(paramList, j);
        if (strcmp(ts_node_type(child), "observer_qualifier") == 0) {
            const TSPoint p = ts_node_start_point(child);
            std::ostringstream err;
            err << "error[semantic/observer_param]: `ref` observers cannot be function "
                   "parameters (line " << (p.row + 1) << ", column " << (p.column + 1)
                << "). Function arguments always transfer ownership (move); pass by value "
                   "and let the caller keep observers local.";
            throw std::runtime_error(err.str());
        }
    }
}

StatementPtr ASTBuilder::buildStatement(TSNode node, const std::string& source) {
    const char* node_type = ts_node_type(node);

    StatementPtr result;
    if (strcmp(node_type, "variable_declaration") == 0) {
        result = buildVariableDeclaration(node, source);
    } else if (strcmp(node_type, "assignment_statement") == 0) {
        result = buildAssignmentStatement(node, source);
    } else if (strcmp(node_type, "expression_statement") == 0) {
        result = buildExpressionStatement(node, source);
    } else if (strcmp(node_type, "comment_statement") == 0) {
        result = buildCommentStatement(node, source);
    } else if (strcmp(node_type, "include_statement") == 0) {
        result = buildIncludeStatement(node, source);
    } else if (strcmp(node_type, "function_declaration") == 0) {
        result = buildFunctionDeclaration(node, source);
    } else if (strcmp(node_type, "class_declaration") == 0) {
        result = buildClassDeclaration(node, source);
    } else if (strcmp(node_type, "constructor_declaration") == 0) {
        result = buildConstructorDeclaration(node, source);
    } else if (strcmp(node_type, "method_declaration") == 0) {
        result = buildMethodDeclaration(node, source);
    } else if (strcmp(node_type, "member_variable_declaration") == 0) {
        result = buildMemberVariableDeclaration(node, source);
    } else if (strcmp(node_type, "main_function") == 0) {
        result = buildMainFunction(node, source);
    } else if (strcmp(node_type, "return_statement") == 0) {
        result = buildReturnStatement(node, source);
    } else if (strcmp(node_type, "if_statement") == 0) {
        result = buildIfStatement(node, source);
    } else if (strcmp(node_type, "statement") == 0) {
        // If we get a generic "statement" node, look at its first child
        if (ts_node_child_count(node) > 0) {
            TSNode child = ts_node_child(node, 0);
            return buildStatement(child, source);
        }
    }

    stampLoc(result.get(), node);
    return result;
}

StatementPtr ASTBuilder::buildVariableDeclaration(TSNode node, const std::string& source) {
    // variable_declaration: optional("ref") type identifier "=" expression ";"
    // The optional `ref` observer_qualifier shifts subsequent child indices by one.
    uint32_t base = 0;
    bool isObserver = false;
    if (ts_node_child_count(node) > 0 &&
        strcmp(ts_node_type(ts_node_child(node, 0)), "observer_qualifier") == 0) {
        isObserver = true;
        base = 1;
    }

    TSNode type_node = ts_node_child(node, base + 0);
    TSNode identifier_node = ts_node_child(node, base + 1);
    TSNode expression_node = ts_node_child(node, base + 3); // Skip "="

    std::string type = getNodeText(type_node, source);
    std::string identifier = getNodeText(identifier_node, source);
    auto expr = buildExpression(expression_node, source);

    return std::make_unique<VariableDeclaration>(type, identifier, std::move(expr), isObserver);
}

StatementPtr ASTBuilder::buildAssignmentStatement(TSNode node, const std::string& source) {
    // assignment_statement: (identifier | member_access) "=" expression ";"
    TSNode lhs_node = ts_node_child(node, 0);
    TSNode expression_node = ts_node_child(node, 2);  // skip "="
    auto expr = buildExpression(expression_node, source);

    if (strcmp(ts_node_type(lhs_node), "member_access") == 0) {
        // Member target `a.b = ...`. Keep the whole MemberAccess as `target`; the root
        // object name (leftmost identifier) goes to `variable` for the analysis passes.
        auto target = buildMemberAccess(lhs_node, source);
        std::string rootName;
        for (const Expression* e = target.get(); e != nullptr; ) {
            if (auto id = dynamic_cast<const Identifier*>(e)) { rootName = id->name; break; }
            if (auto m = dynamic_cast<const MemberAccess*>(e)) { e = m->object.get(); continue; }
            break;
        }
        auto stmt = std::make_unique<AssignmentStatement>(rootName, std::move(expr));
        stmt->target = std::move(target);
        return stmt;
    }

    std::string varName = getNodeText(lhs_node, source);
    return std::make_unique<AssignmentStatement>(varName, std::move(expr));
}

StatementPtr ASTBuilder::buildExpressionStatement(TSNode node, const std::string& source) {
    // expression_statement: expression ";"
    TSNode expression_node = ts_node_child(node, 0);
    auto expr = buildExpression(expression_node, source);
    if (!expr) {
        return nullptr;
    }
    return std::make_unique<ExpressionStatement>(std::move(expr));
}

StatementPtr ASTBuilder::buildCommentStatement(TSNode node, const std::string& source) {
    // comment_statement: comment
    // For a token node, we get the text directly from the node
    std::string commentText = getNodeText(node, source);

    // Remove only the "//" prefix, preserve all following content including whitespace
    if (commentText.length() >= 2 && commentText.substr(0, 2) == "//") {
        commentText = commentText.substr(2);
    }

    return std::make_unique<CommentStatement>(commentText);
}

ExpressionPtr ASTBuilder::buildExpression(TSNode node, const std::string& source) {
    const char* node_type = ts_node_type(node);

    ExpressionPtr result;
    if (strcmp(node_type, "identifier") == 0) {
        result = buildIdentifier(node, source);
    } else if (strcmp(node_type, "scoped_identifier") == 0) {
        result = buildScopedIdentifier(node, source);
    } else if (strcmp(node_type, "string_literal") == 0) {
        result = buildStringLiteral(node, source);
    } else if (strcmp(node_type, "number") == 0) {
        result = buildNumber(node, source);
    } else if (strcmp(node_type, "function_call") == 0) {
        result = buildFunctionCall(node, source);
    } else if (strcmp(node_type, "new_expression") == 0) {
        result = buildNewExpression(node, source);
    } else if (strcmp(node_type, "binary_expression") == 0) {
        result = buildBinaryExpression(node, source);
    } else if (strcmp(node_type, "member_access") == 0) {
        result = buildMemberAccess(node, source);
    } else if (strcmp(node_type, "primary_expression") == 0) {
        // If we get a primary_expression node, look at its first child
        if (ts_node_child_count(node) > 0) {
            TSNode child = ts_node_child(node, 0);
            return buildExpression(child, source);
        }
    } else if (strcmp(node_type, "expression") == 0) {
        // If we get a generic "expression" node, look at its first child
        if (ts_node_child_count(node) > 0) {
            TSNode child = ts_node_child(node, 0);
            return buildExpression(child, source);
        }
    }

    stampLoc(result.get(), node);
    return result;
}

ExpressionPtr ASTBuilder::buildIdentifier(TSNode node, const std::string& source) {
    std::string name = getNodeText(node, source);
    return std::make_unique<Identifier>(name);
}

ExpressionPtr ASTBuilder::buildScopedIdentifier(TSNode node, const std::string& source) {
    // scoped_identifier: identifier ("::" identifier)+ — keep the whole `std::cout` text.
    std::string name = getNodeText(node, source);
    return std::make_unique<ScopedIdentifier>(name);
}

ExpressionPtr ASTBuilder::buildStringLiteral(TSNode node, const std::string& source) {
    // string_literal: a double-quoted token; keep it verbatim (quotes included).
    std::string text = getNodeText(node, source);
    return std::make_unique<StringLiteral>(text);
}

ExpressionPtr ASTBuilder::buildNumber(TSNode node, const std::string& source) {
    std::string value_str = getNodeText(node, source);
    return std::make_unique<Number>(value_str);
}

StatementPtr ASTBuilder::buildFunctionDeclaration(TSNode node, const std::string& source) {
    // function_declaration: type identifier "(" optional(parameter_list) ")" "{" ... "}"
    // C++-style: child(0) is the return type, child(1) is the function name.
    std::string returnType = getNodeText(ts_node_child(node, 0), source);
    std::string funcName = getNodeText(ts_node_child(node, 1), source);

    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
            // Pair each `type` node with the following `identifier` node.
            uint32_t paramCount = ts_node_child_count(child);
            std::string pendingType;
            for (uint32_t j = 0; j < paramCount; j++) {
                TSNode paramNode = ts_node_child(child, j);
                const char* pt = ts_node_type(paramNode);
                if (strcmp(pt, "type") == 0) {
                    pendingType = getNodeText(paramNode, source);
                } else if (strcmp(pt, "identifier") == 0 && !pendingType.empty()) {
                    parameters.push_back({pendingType, getNodeText(paramNode, source)});
                    pendingType.clear();
                }
                // Skip commas / observer_qualifier (already rejected).
            }
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            }
        }
    }

    return std::make_unique<FunctionDeclaration>(returnType, funcName,
                                                 std::move(parameters), std::move(body));
}

ExpressionPtr ASTBuilder::buildFunctionCall(TSNode node, const std::string& source) {
    // function_call: (identifier | member_access) "(" optional(argument_list) ")"
    TSNode calleeNode = ts_node_child(node, 0);
    const bool isMethodCall = strcmp(ts_node_type(calleeNode), "member_access") == 0;

    std::vector<ExpressionPtr> arguments;
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), "argument_list") == 0) {
            uint32_t argCount = ts_node_child_count(child);
            for (uint32_t j = 0; j < argCount; j++) {
                TSNode argNode = ts_node_child(child, j);
                if (strcmp(ts_node_type(argNode), "expression") == 0) {
                    if (auto expr = buildExpression(argNode, source)) {
                        arguments.push_back(std::move(expr));
                    }
                }
            }
        }
    }

    if (isMethodCall) {
        // `recv.method(args)` — keep the receiver MemberAccess as `callee`.
        auto callee = buildMemberAccess(calleeNode, source);
        // function_name is the method name (the member), for readers that ignore callee.
        std::string method = getNodeText(ts_node_child(calleeNode, 2), source);
        auto fc = std::make_unique<FunctionCall>(method, std::move(arguments));
        fc->callee = std::move(callee);
        return fc;
    }

    std::string funcName = getNodeText(calleeNode, source);
    return std::make_unique<FunctionCall>(funcName, std::move(arguments));
}

ExpressionPtr ASTBuilder::buildBinaryExpression(TSNode node, const std::string& source) {
    // binary_expression: primary_expression operator primary_expression
    uint32_t childCount = ts_node_child_count(node);
    
    if (childCount == 2) {
        // Tree-sitter might not include the operator as a separate node
        // Try to extract the operator from the node text
        std::string nodeText = getNodeText(node, source);
        auto left = buildExpression(ts_node_child(node, 0), source);
        auto right = buildExpression(ts_node_child(node, 1), source);
        
        // Check if both expressions were successfully built
        if (!left || !right) {
            return nullptr;
        }
        
        // Find the operator in the text between the expressions
        std::string leftText = getNodeText(ts_node_child(node, 0), source);
        std::string rightText = getNodeText(ts_node_child(node, 1), source);
        
        // Find operator between left and right expressions
        size_t leftEnd = nodeText.find(leftText) + leftText.length();
        size_t rightStart = nodeText.find(rightText);
        std::string op = nodeText.substr(leftEnd, rightStart - leftEnd);
        
        // Trim whitespace
        op.erase(op.begin(), std::find_if(op.begin(), op.end(), [](int ch) { return !std::isspace(ch); }));
        op.erase(std::find_if(op.rbegin(), op.rend(), [](int ch) { return !std::isspace(ch); }).base(), op.end());
        
        return std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
    } else if (childCount == 3) {
        // Traditional case with separate operator node
        auto left = buildExpression(ts_node_child(node, 0), source);
        std::string op = getNodeText(ts_node_child(node, 1), source);
        auto right = buildExpression(ts_node_child(node, 2), source);
        
        // Check if both expressions were successfully built
        if (!left || !right) {
            return nullptr;
        }
        
        return std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
    }
    
    return nullptr;
}

StatementPtr ASTBuilder::buildReturnStatement(TSNode node, const std::string& source) {
    // return_statement: "return" expression ";"
    TSNode expression_node = ts_node_child(node, 1); // Skip "return"
    auto expr = buildExpression(expression_node, source);
    return std::make_unique<ReturnStatement>(std::move(expr));
}

StatementPtr ASTBuilder::buildMainFunction(TSNode node, const std::string& source) {
    // main_function: type "main" "(" ")" "{" repeat(statement) "}"
    TSNode type_node = ts_node_child(node, 0);
    std::string returnType = getNodeText(type_node, source);

    std::vector<StatementPtr> body;

    // Parse body statements
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            }
        }
    }

    return std::make_unique<MainFunction>(returnType, std::move(body));
}

ExpressionPtr ASTBuilder::buildNewExpression(TSNode node, const std::string& source) {
    // new_expression: "new" identifier "(" optional(argument_list) ")"
    TSNode class_node = ts_node_child(node, 1); // Skip "new"
    std::string className = getNodeText(class_node, source);

    std::vector<ExpressionPtr> arguments;

    // Parse arguments if they exist
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "argument_list") == 0) {
            uint32_t argCount = ts_node_child_count(child);
            for (uint32_t j = 0; j < argCount; j++) {
                TSNode argNode = ts_node_child(child, j);
                if (strcmp(ts_node_type(argNode), "expression") == 0) {
                    auto expr = buildExpression(argNode, source);
                    if (expr) {
                        arguments.push_back(std::move(expr));
                    }
                }
                // Skip comma tokens
            }
        }
    }

    return std::make_unique<NewExpression>(className, std::move(arguments));
}

StatementPtr ASTBuilder::buildIncludeStatement(TSNode node, const std::string& source) {
    // include_statement: "#include" choice(seq('<', /[^>]+/, '>'), seq('"', /[^"]+/, '"'))
    std::string includeText = getNodeText(node, source);
    return std::make_unique<IncludeStatement>(includeText);
}

StatementPtr ASTBuilder::buildClassDeclaration(TSNode node, const std::string& source) {
    // class_declaration: "class" identifier "{" "public" ":" repeat(member_variable_declaration) constructor_declaration "}" optional(";")
    TSNode name_node = ts_node_child(node, 1); // Skip "class"
    std::string className = getNodeText(name_node, source);

    std::vector<StatementPtr> members;

    // Parse all children to find class members
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "member_variable_declaration") == 0 ||
            strcmp(childType, "constructor_declaration") == 0 ||
            strcmp(childType, "method_declaration") == 0) {
            // Direct member
            auto member = buildStatement(child, source);
            if (member) {
                members.push_back(std::move(member));
            }
        }
    }

    return std::make_unique<ClassDeclaration>(className, std::move(members));
}

StatementPtr ASTBuilder::buildConstructorDeclaration(TSNode node, const std::string& source) {
    // constructor_declaration: identifier "(" optional(parameter_list) ")" "{" repeat(statement) "}"
    TSNode name_node = ts_node_child(node, 0);
    std::string constructorName = getNodeText(name_node, source);

    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    // Parse all children to find parameters and body
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
            // Parse parameters: seq(type, identifier), repeat(seq(',', type, identifier))
            uint32_t paramCount = ts_node_child_count(child);
            for (uint32_t j = 0; j < paramCount; j += 2) { // Skip commas
                TSNode paramChild = ts_node_child(child, j);
                const char* paramChildType = ts_node_type(paramChild);

                if (strcmp(paramChildType, "type") == 0 && j + 1 < paramCount) {
                    TSNode typeNode = paramChild;
                    TSNode identifierNode = ts_node_child(child, j + 1);
                    if (strcmp(ts_node_type(identifierNode), "identifier") == 0) {
                        std::string type = getNodeText(typeNode, source);
                        std::string name = getNodeText(identifierNode, source);
                        parameters.push_back({type, name});
                    }
                }
            }
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            }
        }
    }

    return std::make_unique<ConstructorDeclaration>(constructorName, std::move(parameters), std::move(body));
}

StatementPtr ASTBuilder::buildMemberVariableDeclaration(TSNode node, const std::string& source) {
    // member_variable_declaration: type identifier ";"
    TSNode type_node = ts_node_child(node, 0);
    TSNode identifier_node = ts_node_child(node, 1);

    std::string type = getNodeText(type_node, source);
    std::string identifier = getNodeText(identifier_node, source);

    // For member variables without initialization, we'll use a null expression
    return std::make_unique<VariableDeclaration>(type, identifier, nullptr);
}

StatementPtr ASTBuilder::buildIfStatement(TSNode node, const std::string& source) {
    // if_statement: "if" "(" expression ")" "{" stmts "}"
    //               [ "else" ( if_statement | "{" stmts "}" ) ]
    TSNode condition_node = ts_node_child(node, 2); // Skip "if" and "("
    auto condition = buildExpression(condition_node, source);

    std::vector<StatementPtr> body;
    std::vector<StatementPtr> elseBody;

    // The token `else` separates the then-statements from the else-branch. Statements
    // (and a nested if_statement for `else if`) before it go to body, after it to else.
    uint32_t childCount = ts_node_child_count(node);
    bool afterElse = false;
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (getNodeText(child, source) == "else") { afterElse = true; continue; }

        const bool isStmt = strcmp(childType, "statement") == 0;
        const bool isNestedIf = strcmp(childType, "if_statement") == 0;  // `else if`
        if (!isStmt && !isNestedIf) continue;

        auto built = buildStatement(child, source);
        if (!built) continue;
        (afterElse ? elseBody : body).push_back(std::move(built));
    }

    auto result = std::make_unique<IfStatement>(std::move(condition), std::move(body));
    result->elseBody = std::move(elseBody);
    return result;
}

ExpressionPtr ASTBuilder::buildMemberAccess(TSNode node, const std::string& source) {
    // member_access: primary_expression "->" identifier
    TSNode object_node = ts_node_child(node, 0);
    TSNode member_node = ts_node_child(node, 2); // Skip "->"

    auto object = buildExpression(object_node, source);
    std::string member = getNodeText(member_node, source);

    return std::make_unique<MemberAccess>(std::move(object), member);
}

StatementPtr ASTBuilder::buildMethodDeclaration(TSNode node, const std::string& source) {
    // method_declaration: type identifier "(" optional(parameter_list) ")" "{" repeat(statement) "}"
    TSNode return_type_node = ts_node_child(node, 0);
    TSNode name_node = ts_node_child(node, 1);
    std::string returnType = getNodeText(return_type_node, source);
    std::string methodName = getNodeText(name_node, source);

    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    // Parse all children to find parameters and body
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
            // Parse parameters: seq(type, identifier), repeat(seq(',', type, identifier))
            uint32_t paramCount = ts_node_child_count(child);
            for (uint32_t j = 0; j < paramCount; j += 2) { // Skip commas
                TSNode paramChild = ts_node_child(child, j);
                const char* paramChildType = ts_node_type(paramChild);

                if (strcmp(paramChildType, "type") == 0 && j + 1 < paramCount) {
                    TSNode typeNode = paramChild;
                    TSNode identifierNode = ts_node_child(child, j + 1);
                    if (strcmp(ts_node_type(identifierNode), "identifier") == 0) {
                        std::string type = getNodeText(typeNode, source);
                        std::string name = getNodeText(identifierNode, source);
                        parameters.push_back({type, name});
                    }
                }
            }
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            }
        }
    }

    return std::make_unique<MethodDeclaration>(returnType, methodName, std::move(parameters), std::move(body));
}

} // namespace cling
