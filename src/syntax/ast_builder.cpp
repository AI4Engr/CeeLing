#include "syntax/ast_builder.h"
#include "facts/library_summary.h"
#include <stdexcept>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace cling {

namespace {

// If `snippet` contains a construct the language forbids (casts, union, delete), returns
// a suggestion saying why. These have no grammar production, so they reach here as
// unbuildable nodes.
std::string forbiddenConstructSuggestion(const std::string& snippet) {
    if (snippet.find("reinterpret_cast") != std::string::npos ||
        snippet.find("const_cast") != std::string::npos ||
        snippet.find("static_cast") != std::string::npos) {
        return " Suggestion: C++-style casts are forbidden in CeeLing (guide chapter 13, hatch "
               "#13/#16) -- casting is a type-confusion / const-bypass escape hatch "
               "with no safe equivalent, so there is no cast syntax in the language.";
    }
    if (snippet.find("union") != std::string::npos) {
        return " Suggestion: raw `union` is forbidden (hatch #17) -- it allows reading "
               "a different member than was written, which is undefined behavior C++ "
               "cannot catch. Use a class with distinct fields instead.";
    }
    if (snippet.find("delete") != std::string::npos) {
        return " Suggestion: raw `delete` is forbidden (hatch #15) -- object lifetime "
               "is managed automatically by the compiler-chosen smart pointer; there is "
               "no manual delete.";
    }
    // `T* name` parses as a multiplication, so EscapeHatchChecker rejects it, not this function.
    return "";
}

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

    if (std::string forbidden = forbiddenConstructSuggestion(snippet); !forbidden.empty()) {
        oss << forbidden;
    } else if (snippet.find(":=") != std::string::npos) {
        oss << ". Suggestion: use '=' for assignment.";
    } else if (!snippet.empty() && snippet.find(';') == std::string::npos &&
               snippet.find('{') == std::string::npos && snippet.find('}') == std::string::npos) {
        oss << ". Suggestion: check whether the statement is missing a trailing ';'.";
    } else if (snippet.find("->") != std::string::npos || snippet.find('.') != std::string::npos) {
        oss << ". Suggestion: check member-access syntax and make sure the full statement is supported.";
    }
    return oss.str();
}

// A body loop hit an ERROR/missing node or a statement that failed to build. The source
// holds something unsupported; raise an error rather than drop it, which would hide it
// from the checkers.
[[noreturn]] void rejectUnbuildableChild(TSNode child, const std::string& source) {
    throw std::runtime_error(makeNodeError("Parse error", child, source));
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

// Sets a node's source position from its CST node.
static void stampLoc(ASTNode* astNode, TSNode tsNode) {
    if (astNode == nullptr) return;
    const TSPoint p = ts_node_start_point(tsNode);
    astNode->loc.line = static_cast<int>(p.row) + 1;
    astNode->loc.column = static_cast<int>(p.column) + 1;
}

// A specialization's target class may be written bare (`Node`) or qualified (`YAML::Node`).
static bool classMatchesSpecializationTarget(const ClassDeclaration& cls, const std::string& targetClass) {
    if (cls.name == targetClass) return true;
    return !cls.namespaceName.empty() && (cls.namespaceName + "::" + cls.name) == targetClass;
}

ProgramPtr ASTBuilder::buildProgram(const std::string& source) {
    auto program = std::make_unique<Program>();

    TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(), source.length());
    if (!tree) {
        throw std::runtime_error("Failed to parse source with Tree-sitter");
    }

    TSNode root_node = ts_tree_root_node(tree);
    // Tree-sitter recovers from syntax errors by dropping or inventing tokens, and the
    // builder would build from what is left. Any ERROR or MISSING node is a syntax error.
    if (ts_node_has_error(root_node)) {
        std::vector<TSNode> pending{root_node};
        TSNode firstBad{};
        bool found = false;
        while (!pending.empty() && !found) {
            TSNode n = pending.back();
            pending.pop_back();
            if (strcmp(ts_node_type(n), "ERROR") == 0 || ts_node_is_missing(n)) {
                firstBad = n;
                found = true;
                break;
            }
            if (!ts_node_has_error(n)) continue;
            // pushed in reverse so the earliest child is examined first
            for (int64_t i = static_cast<int64_t>(ts_node_child_count(n)) - 1; i >= 0; --i) {
                pending.push_back(ts_node_child(n, static_cast<uint32_t>(i)));
            }
        }
        // build the message before freeing the tree; the node points into it
        const std::string message = found ? makeNodeError("Syntax error", firstBad, source) : std::string("Syntax error");
        ts_tree_delete(tree);
        throw std::runtime_error(message);
    }
    bool hasStructuralAnchor = false;
    bool hasUnresolvedTopLevel = false;
    TSNode firstUnresolvedNode{};
    // Top-level external method specializations, spliced into their target class after
    // all statements are built, so declaration order does not matter.
    std::vector<std::pair<std::string, StatementPtr>> pendingExternalSpecializations;

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

        // A top-level comment is a bare `comment` child (a grammar extra), not wrapped
        // in a `statement`; buildStatement would not build it.
        if (strcmp(ts_node_type(child), "comment") == 0) {
            continue;
        }

        // `namespace X { ... }` becomes several top-level statements, so it is handled
        // here and not in buildStatement (top-level namespaces only). Top-level children
        // are wrapped in a "statement" node; unwrap it to see the real node type.
        TSNode inner = child;
        if (strcmp(ts_node_type(inner), "statement") == 0 && ts_node_child_count(inner) > 0) {
            inner = ts_node_child(inner, 0);
        }
        if (strcmp(ts_node_type(inner), "namespace_declaration") == 0) {
            for (auto& desugared : buildNamespaceDeclaration(inner, source)) {
                if (dynamic_cast<ClassDeclaration*>(desugared.get())) {
                    hasStructuralAnchor = true;
                }
                program->addStatement(std::move(desugared));
            }
            continue;
        }

        // `ReturnType ClassName::methodName<Type>(...) {...}` at top level produces a
        // method that is spliced into an existing class's members, so it is collected
        // here and spliced after the main loop.
        if (strcmp(ts_node_type(inner), "external_method_specialization") == 0) {
            std::string targetClass;
            auto methodDecl = buildExternalMethodSpecialization(inner, source, targetClass);
            stampLoc(methodDecl.get(), inner);
            // Take the raw pointer before ownership moves away. The marker fixes the
            // emission position; the splice target is what the checkers see.
            auto* methodDeclRaw = static_cast<MethodDeclaration*>(methodDecl.get());
            auto marker = std::make_unique<ExternalSpecializationMarker>(targetClass, methodDeclRaw);
            stampLoc(marker.get(), inner);
            program->addStatement(std::move(marker));
            pendingExternalSpecializations.emplace_back(targetClass, std::move(methodDecl));
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

    // Splice each specialization into its target class's members, so every pass sees
    // it as an ordinary method. A target not in this file may come from an #include'd
    // file merged later, so it is deferred to unresolvedExternalSpecializations; the
    // caller retries after merging and an entry still unresolved then is an error.
    for (auto& [targetClass, methodDecl] : pendingExternalSpecializations) {
        ClassDeclaration* target = nullptr;
        for (auto& stmt : program->statements) {
            if (auto cls = dynamic_cast<ClassDeclaration*>(stmt.get());
                cls != nullptr && classMatchesSpecializationTarget(*cls, targetClass)) {
                target = cls;
                break;
            }
        }
        if (target == nullptr) {
            program->unresolvedExternalSpecializations.emplace_back(targetClass, std::move(methodDecl));
            continue;
        }
        target->members.push_back(std::move(methodDecl));
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
    validateInheritance(*program);
    return program;
}

// A base clause must name a class of this program and the graph must be acyclic;
// the passes flatten inherited members base first and cannot cope with either.
void ASTBuilder::validateInheritance(const Program& program) {
    std::map<std::string, const ClassDeclaration*> classes;
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classes[cls->name] = cls;
        }
    }

    for (const auto& [name, cls] : classes) {
        if (cls->baseClass.empty()) continue;

        if (classes.find(cls->baseClass) == classes.end()) {
            std::ostringstream oss;
            oss << "error[semantic/unknown-base-class]: class `" << name
                << "` inherits from `" << cls->baseClass << "`, which is not a class "
                   "declared in this program (line " << cls->loc.line << "). CeeLing "
                   "cannot inherit from anything it did not compile itself -- an "
                   "external base class has no known field/method set, so its members "
                   "could not be resolved or safety-checked.";
            throw std::runtime_error(oss.str());
        }

        // Walk up the chain; meeting a class twice means a cycle.
        std::set<std::string> seen{name};
        std::string cursor = cls->baseClass;
        while (!cursor.empty()) {
            if (!seen.insert(cursor).second) {
                std::ostringstream oss;
                oss << "error[semantic/inheritance-cycle]: class `" << name
                    << "` is part of an inheritance cycle (line " << cls->loc.line
                    << "). A class cannot directly or indirectly inherit from itself.";
                throw std::runtime_error(oss.str());
            }
            auto it = classes.find(cursor);
            if (it == classes.end()) break;   // unknown base already reported above
            cursor = it->second->baseClass;
        }
    }
}


// A `ref` observer may not be a parameter; arguments always transfer ownership.
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
    } else if (strcmp(node_type, "ext_fn_declaration") == 0) {
        result = buildExternFunctionDeclaration(node, source);
    } else if (strcmp(node_type, "class_declaration") == 0) {
        result = buildClassDeclaration(node, source);
    } else if (strcmp(node_type, "enum_declaration") == 0) {
        result = buildEnumDeclaration(node, source);
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
    } else if (strcmp(node_type, "while_statement") == 0) {
        result = buildWhileStatement(node, source);
    } else if (strcmp(node_type, "for_statement") == 0) {
        result = buildForStatement(node, source);
    } else if (strcmp(node_type, "break_statement") == 0) {
        result = std::make_unique<BreakStatement>();
    } else if (strcmp(node_type, "continue_statement") == 0) {
        result = std::make_unique<ContinueStatement>();
    } else if (strcmp(node_type, "throw_statement") == 0) {
        result = buildThrowStatement(node, source);
    } else if (strcmp(node_type, "try_statement") == 0) {
        result = buildTryStatement(node, source);
    } else if (strcmp(node_type, "container_declaration") == 0) {
        result = buildContainerDeclaration(node, source);
    } else if (strcmp(node_type, "statement") == 0) {
        // generic wrapper: build its first child
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
    // A leading `ref` shifts the child indices by one.
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

StatementPtr ASTBuilder::buildContainerDeclaration(TSNode node, const std::string& source) {
    // container_declaration: generic_type identifier ";"
    TSNode type_node = ts_node_child(node, 0);
    TSNode identifier_node = ts_node_child(node, 1);

    // `std::vector<ref Node>`: the grammar has no `ref` inside type arguments, so it
    // leaves an ERROR node under generic_type. Container-held observers are forbidden
    // (an element has no reliable provenance), so give a targeted message.
    if (ts_node_has_error(type_node)) {
        std::ostringstream oss;
        const TSPoint p = ts_node_start_point(node);
        oss << "error[semantic/forbidden-construct]: `ref` cannot appear inside a "
               "container's element type (line " << (p.row + 1) << ", column "
            << (p.column + 1) << "). A container element has no independent "
               "identity to observe safely -- storing a `ref` inside `std::vector<...>` "
               "would let an observer's lifetime outlive its owner anonymously; "
               "declare the container's element type as a plain owning type instead.";
        throw std::runtime_error(oss.str());
    }

    std::string type = getNodeText(type_node, source);
    std::string identifier = getNodeText(identifier_node, source);

    // `std::map<K,V>` takes primitive keys and values only: an object entry would need an
    // ownership decision per entry.
    if (isMapType(type)) {
        const std::string keyType = mapKeyType(type);
        const std::string valueType = mapValueType(type);
        if (!isPrimitiveType(keyType) || !isPrimitiveType(valueType)) {
            std::ostringstream oss;
            const TSPoint p = ts_node_start_point(node);
            oss << "error[semantic/forbidden-construct]: `std::map<" << keyType << ", "
                << valueType << ">` is not supported (line " << (p.row + 1)
                << ", column " << (p.column + 1) << "). CeeLing's std::map support is "
                   "restricted to primitive key/value types (int, float, double, bool, "
                   "string) for now -- class-typed map entries need an element-ownership "
                   "decision (unique_ptr vs shared_ptr) that isn't implemented yet.";
            throw std::runtime_error(oss.str());
        }
    }

    return std::make_unique<ContainerDeclaration>(type, identifier);
}

StatementPtr ASTBuilder::buildAssignmentStatement(TSNode node, const std::string& source) {
    // assignment_statement: (identifier | member_access | subscript_expression) "=" expression ";"
    TSNode lhs_node = ts_node_child(node, 0);
    TSNode expression_node = ts_node_child(node, 2);  // skip "="
    auto expr = buildExpression(expression_node, source);

    if (strcmp(ts_node_type(lhs_node), "member_access") == 0) {
        // Member target `a.b = ...`: the MemberAccess is `target`, the leftmost identifier `variable`.
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

    if (strcmp(ts_node_type(lhs_node), "subscript_expression") == 0) {
        // Subscript target `m[key] = value;`. `object` is a bare container name, so no
        // walk is needed for the root.
        auto target = buildSubscriptExpression(lhs_node, source);
        std::string rootName;
        if (auto sub = dynamic_cast<const SubscriptExpression*>(target.get())) {
            if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) {
                rootName = id->name;
            }
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
    std::string commentText = getNodeText(node, source);

    // drop the "//" prefix only
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
    } else if (strcmp(node_type, "this_expression") == 0) {
        result = std::make_unique<ThisExpression>();
    } else if (strcmp(node_type, "scoped_identifier") == 0) {
        result = buildScopedIdentifier(node, source);
    } else if (strcmp(node_type, "string_literal") == 0) {
        result = buildStringLiteral(node, source);
    } else if (strcmp(node_type, "char_literal") == 0) {
        result = buildCharLiteral(node, source);
    } else if (strcmp(node_type, "number") == 0) {
        result = buildNumber(node, source);
    } else if (strcmp(node_type, "function_call") == 0) {
        result = buildFunctionCall(node, source);
    } else if (strcmp(node_type, "template_call") == 0) {
        result = buildTemplateCall(node, source);
    } else if (strcmp(node_type, "new_expression") == 0) {
        result = buildNewExpression(node, source);
    } else if (strcmp(node_type, "closure_expression") == 0) {
        result = buildClosureExpression(node, source);
    } else if (strcmp(node_type, "binary_expression") == 0) {
        result = buildBinaryExpression(node, source);
    } else if (strcmp(node_type, "unary_expression") == 0) {
        result = buildUnaryExpression(node, source);
    } else if (strcmp(node_type, "member_access") == 0) {
        result = buildMemberAccess(node, source);
    } else if (strcmp(node_type, "subscript_expression") == 0) {
        result = buildSubscriptExpression(node, source);
    } else if (strcmp(node_type, "primary_expression") == 0) {
        // The grouped form `( expression )` has three children, so take the named child
        // (skips the parentheses).
        if (ts_node_named_child_count(node) > 0) {
            TSNode child = ts_node_named_child(node, 0);
            return buildExpression(child, source);
        }
    } else if (strcmp(node_type, "expression") == 0) {
        // generic wrapper: build its first child
        if (ts_node_child_count(node) > 0) {
            TSNode child = ts_node_child(node, 0);
            return buildExpression(child, source);
        }
    }

    // An unrecognized node, or a builder that failed, must not return null: callers
    // assume a result, and a dropped operand would hide a forbidden construct.
    if (!result) {
        rejectUnbuildableChild(node, source);
    }

    stampLoc(result.get(), node);
    return result;
}

ExpressionPtr ASTBuilder::buildIdentifier(TSNode node, const std::string& source) {
    std::string name = getNodeText(node, source);
    return std::make_unique<Identifier>(name);
}

ExpressionPtr ASTBuilder::buildScopedIdentifier(TSNode node, const std::string& source) {
    std::string name = getNodeText(node, source);
    return std::make_unique<ScopedIdentifier>(name);
}

namespace {

// Rejects an escape sequence outside a whitelist. The literal is emitted verbatim, and C++
// gives unknown escapes (`\q`) an implementation-defined meaning and `\uXXXX` a result that
// depends on the execution character set; use `\xHH` for a byte. `\0` may not be followed by
// an octal digit, and `\xHH` is exactly two hex digits so it cannot swallow following text.
void checkLiteralEscapes(const std::string& text, bool isString, TSNode node, const std::string& source) {
    auto fail = [&](const std::string& what) {
        throw std::runtime_error(
            makeNodeError(("Invalid escape sequence in literal: " + what).c_str(), node, source));
    };
    // skip the quotes
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] != '\\') continue;
        const char e = text[i + 1];
        switch (e) {
            case 'n': case 'r': case 't': case 'b': case 'f': case '\\': ++i; break;
            case '0':
                if (i + 2 < text.size() - 1 && text[i + 2] >= '0' && text[i + 2] <= '7') {
                    fail("`\\0` followed by another octal digit is an octal escape; write `\\xHH` instead");
                }
                ++i;
                break;
            case 'x': {
                auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
                if (i + 3 >= text.size() || !hex(text[i + 2]) || !hex(text[i + 3])) {
                    fail("`\\x` must be followed by exactly two hex digits");
                }
                i += 3;
                break;
            }
            case '"': ++i; break;                     // valid in both literal kinds in C++ ('\"' is a quote char)
            case '\'':
                // `\'` belongs in a char literal; in a string a plain `'` does.
                if (isString) fail("`\\'` is only valid in a character literal; use a plain `'`");
                ++i;
                break;
            default:
                if (e == 'u' || e == 'U') {
                    fail("`\\u`/`\\U` are not supported (the bytes they produce depend on the execution "
                         "character set); write the UTF-8 bytes with `\\xHH`");
                }
                fail(std::string("unknown escape `\\") + e + "`");
        }
    }
}

} // namespace

ExpressionPtr ASTBuilder::buildStringLiteral(TSNode node, const std::string& source) {
    std::string text = getNodeText(node, source);
    checkLiteralEscapes(text, /*isString=*/true, node, source);
    return std::make_unique<StringLiteral>(text);
}

ExpressionPtr ASTBuilder::buildCharLiteral(TSNode node, const std::string& source) {
    std::string text = getNodeText(node, source);
    checkLiteralEscapes(text, /*isString=*/false, node, source);
    return std::make_unique<CharLiteral>(text);
}

ExpressionPtr ASTBuilder::buildUnaryExpression(TSNode node, const std::string& source) {
    // The operator token ('!', '-', '*' or '++') is inlined, so recover it from the
    // leading text; '++' has two characters.
    TSNode operand_node = ts_node_child(node, ts_node_child_count(node) - 1);
    auto operand = buildExpression(operand_node, source);
    if (!operand) return nullptr;

    std::string full = getNodeText(node, source);
    std::string op = full.empty() ? "!"
        : (full.rfind("++", 0) == 0 ? "++" : std::string(1, full[0]));
    return std::make_unique<UnaryExpression>(op, std::move(operand));
}

ExpressionPtr ASTBuilder::buildNumber(TSNode node, const std::string& source) {
    std::string value_str = getNodeText(node, source);
    return std::make_unique<Number>(value_str);
}

StatementPtr ASTBuilder::buildFunctionDeclaration(TSNode node, const std::string& source) {
    // child(0) is the return type, child(1) the name
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
            // pair each `type` with the following `identifier`
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
        } else if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            } else {
                rejectUnbuildableChild(child, source);
            }
        }
    }

    return std::make_unique<FunctionDeclaration>(returnType, funcName,
                                                 std::move(parameters), std::move(body));
}

StatementPtr ASTBuilder::buildExternFunctionDeclaration(TSNode node, const std::string& source) {
    // The first child is the 'ext_fn' keyword, so type and name are found by node type,
    // not by index.
    std::string returnType;
    std::string funcName;
    bool sawReturnType = false;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::set<size_t> refParams;
    int borrowsParam = -1;

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "type") == 0 && !sawReturnType) {
            returnType = getNodeText(child, source);
            sawReturnType = true;
        } else if (strcmp(childType, "identifier") == 0 && sawReturnType && funcName.empty()) {
            funcName = getNodeText(child, source);
        } else if (strcmp(childType, "parameter_list") == 0) {
            // ext_fn is the one place a parameter may be `ref`, so rejectObserverParams is skipped.
            uint32_t paramCount = ts_node_child_count(child);
            std::string pendingType;
            bool pendingIsRef = false;
            for (uint32_t j = 0; j < paramCount; j++) {
                TSNode paramNode = ts_node_child(child, j);
                const char* pt = ts_node_type(paramNode);
                if (strcmp(pt, "observer_qualifier") == 0) {
                    pendingIsRef = true;
                } else if (strcmp(pt, "type") == 0) {
                    pendingType = getNodeText(paramNode, source);
                } else if (strcmp(pt, "identifier") == 0 && !pendingType.empty()) {
                    if (pendingIsRef) refParams.insert(parameters.size());
                    parameters.push_back({pendingType, getNodeText(paramNode, source)});
                    pendingType.clear();
                    pendingIsRef = false;
                }
            }
        } else if (strcmp(childType, "number") == 0) {
            // the N of `borrows N`, the only direct number child
            borrowsParam = std::stoi(getNodeText(child, source));
        } else if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        }
    }

    // Parameters and return must be primitive or std::vector<primitive>: an object type
    // would need borrow facts richer than a single `borrows N`.
    auto isSupportedExternType = [](const std::string& type) {
        if (isPrimitiveType(type)) return true;
        if (isVectorType(type)) return isPrimitiveType(vectorElementType(type));
        return false;
    };
    auto rejectUnsupportedType = [&](const std::string& type, const std::string& context) {
        if (isSupportedExternType(type)) return;
        std::ostringstream oss;
        const TSPoint p = ts_node_start_point(node);
        oss << "error[semantic/forbidden-construct]: ext_fn " << context << " type `"
            << type << "` is not supported (line " << (p.row + 1) << ", column "
            << (p.column + 1) << "). ext_fn signatures are restricted to primitive "
               "types (int, float, double, bool, string, char) and "
               "std::vector<primitive> for now -- a class-typed ext_fn parameter or "
               "return needs a hand-written borrow-summary annotation that isn't "
               "designed yet (see guide chapter 11).";
        throw std::runtime_error(oss.str());
    };
    rejectUnsupportedType(returnType, "return");
    for (const auto& [paramType, paramName] : parameters) {
        rejectUnsupportedType(paramType, "parameter `" + paramName + "`");
    }
    // `ref` is valid on primitives and vectors of primitives; this loop is the place to
    // reject any other type that may become allowed in ext_fn later.
    for (size_t i = 0; i < parameters.size(); ++i) {
        if (!refParams.count(i)) continue;
        const auto& [paramType, paramName] = parameters[i];
        if (isPrimitiveType(paramType)) continue;
        if (isVectorType(paramType) && isPrimitiveType(vectorElementType(paramType))) continue;
        std::ostringstream oss;
        const TSPoint p = ts_node_start_point(node);
        oss << "error[semantic/forbidden-construct]: ext_fn `ref` parameter `"
            << paramName << "` has unsupported type `" << paramType
            << "` (line " << (p.row + 1) << ", column " << (p.column + 1)
            << "). `ref` is only supported on primitive types (mutable "
               "out-param) and std::vector<primitive> (read-only borrow).";
        throw std::runtime_error(oss.str());
    }
    if (borrowsParam >= static_cast<int>(parameters.size())) {
        std::ostringstream oss;
        const TSPoint p = ts_node_start_point(node);
        oss << "error[semantic/forbidden-construct]: ext_fn `" << funcName
            << "` declares `borrows " << borrowsParam << "` but has only "
            << parameters.size() << " parameter(s) (line " << (p.row + 1)
            << ", column " << (p.column + 1) << ").";
        throw std::runtime_error(oss.str());
    }

    auto decl = std::make_unique<ExternFunctionDeclaration>(returnType, funcName,
                                                             std::move(parameters), borrowsParam);
    decl->refParams = std::move(refParams);
    return decl;
}

ExpressionPtr ASTBuilder::buildFunctionCall(TSNode node, const std::string& source) {
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
        // `recv.method(args)`: the MemberAccess is `callee`, function_name the method name.
        auto callee = buildMemberAccess(calleeNode, source);
        std::string method = getNodeText(ts_node_child(calleeNode, 2), source);
        auto fc = std::make_unique<FunctionCall>(method, std::move(arguments));
        fc->callee = std::move(callee);
        return fc;
    }

    std::string funcName = getNodeText(calleeNode, source);
    return std::make_unique<FunctionCall>(funcName, std::move(arguments));
}

ExpressionPtr ASTBuilder::buildTemplateCall(TSNode node, const std::string& source) {
    // Builds a FunctionCall with templateArg set; the passes pick the lookup key from it.
    TSNode calleeNode = ts_node_child(node, 0);
    const bool isMethodCall = strcmp(ts_node_type(calleeNode), "member_access") == 0;

    std::string templateArg;
    std::vector<ExpressionPtr> arguments;
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);
        if (strcmp(childType, "type") == 0) {
            templateArg = getNodeText(child, source);
        } else if (strcmp(childType, "argument_list") == 0) {
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

    ExpressionPtr result;
    if (isMethodCall) {
        auto callee = buildMemberAccess(calleeNode, source);
        std::string method = getNodeText(ts_node_child(calleeNode, 2), source);
        auto fc = std::make_unique<FunctionCall>(method, std::move(arguments));
        fc->callee = std::move(callee);
        fc->templateArg = templateArg;
        result = std::move(fc);
    } else {
        // `f<int>(...)` or a subscript callee `v[i]<int>(...)`
        std::string funcName = getNodeText(calleeNode, source);
        auto fc = std::make_unique<FunctionCall>(funcName, std::move(arguments));
        fc->templateArg = templateArg;
        result = std::move(fc);
    }
    return result;
}

ExpressionPtr ASTBuilder::buildBinaryExpression(TSNode node, const std::string& source) {
    uint32_t childCount = ts_node_child_count(node);

    if (childCount == 2) {
        // The operator is not a separate node; recover it from the text between the operands.
        std::string nodeText = getNodeText(node, source);
        auto left = buildExpression(ts_node_child(node, 0), source);
        auto right = buildExpression(ts_node_child(node, 1), source);
        
        if (!left || !right) {
            return nullptr;
        }

        std::string leftText = getNodeText(ts_node_child(node, 0), source);
        std::string rightText = getNodeText(ts_node_child(node, 1), source);

        size_t leftEnd = nodeText.find(leftText) + leftText.length();
        size_t rightStart = nodeText.find(rightText);
        std::string op = nodeText.substr(leftEnd, rightStart - leftEnd);

        // trim whitespace
        op.erase(op.begin(), std::find_if(op.begin(), op.end(), [](int ch) { return !std::isspace(ch); }));
        op.erase(std::find_if(op.rbegin(), op.rend(), [](int ch) { return !std::isspace(ch); }).base(), op.end());
        
        return std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
    } else if (childCount == 3) {
        // the operator is its own node
        auto left = buildExpression(ts_node_child(node, 0), source);
        std::string op = getNodeText(ts_node_child(node, 1), source);
        auto right = buildExpression(ts_node_child(node, 2), source);

        if (!left || !right) {
            return nullptr;
        }
        
        return std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
    }
    
    return nullptr;
}

StatementPtr ASTBuilder::buildReturnStatement(TSNode node, const std::string& source) {
    // a bare `return;` has no named child
    if (ts_node_named_child_count(node) == 0) {
        return std::make_unique<ReturnStatement>(nullptr);
    }
    TSNode expression_node = ts_node_named_child(node, 0);
    auto expr = buildExpression(expression_node, source);
    return std::make_unique<ReturnStatement>(std::move(expr));
}

StatementPtr ASTBuilder::buildThrowStatement(TSNode node, const std::string& source) {
    TSNode expression_node = ts_node_child(node, 1); // skip "throw"
    auto expr = buildExpression(expression_node, source);
    return std::make_unique<ThrowStatement>(std::move(expr));
}

StatementPtr ASTBuilder::buildTryStatement(TSNode node, const std::string& source) {
    // The try and catch bodies are flat siblings; split them on the "catch" keyword.
    std::vector<StatementPtr> body;
    std::vector<StatementPtr> catchBody;
    std::string catchType;
    std::string catchIdentifier;

    uint32_t childCount = ts_node_child_count(node);
    bool afterCatch = false;
    bool sawType = false;
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (getNodeText(child, source) == "catch") { afterCatch = true; continue; }

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        }

        if (afterCatch && strcmp(childType, "type") == 0 && !sawType) {
            catchType = getNodeText(child, source);
            sawType = true;
            continue;
        }
        if (afterCatch && strcmp(childType, "identifier") == 0 && catchIdentifier.empty()) {
            catchIdentifier = getNodeText(child, source);
            continue;
        }

        if (strcmp(childType, "statement") != 0) continue;
        auto built = buildStatement(child, source);
        if (!built) rejectUnbuildableChild(child, source);
        (afterCatch ? catchBody : body).push_back(std::move(built));
    }

    return std::make_unique<TryStatement>(std::move(body), catchType, catchIdentifier,
                                          std::move(catchBody));
}

StatementPtr ASTBuilder::buildMainFunction(TSNode node, const std::string& source) {
    TSNode type_node = ts_node_child(node, 0);
    std::string returnType = getNodeText(type_node, source);

    std::vector<StatementPtr> body;

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            } else {
                rejectUnbuildableChild(child, source);
            }
        }
    }

    return std::make_unique<MainFunction>(returnType, std::move(body));
}

ExpressionPtr ASTBuilder::buildNewExpression(TSNode node, const std::string& source) {
    // className keeps a qualifier as written (`new YAML::Node(...)` gives "YAML::Node");
    // callers that need the simple name strip it.
    TSNode class_node = ts_node_child(node, 1); // skip "new"
    std::string className = getNodeText(class_node, source);

    std::vector<ExpressionPtr> arguments;

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
            }
        }
    }

    return std::make_unique<NewExpression>(className, std::move(arguments));
}

ExpressionPtr ASTBuilder::buildClosureExpression(TSNode node, const std::string& source) {
    // Builds the transient ClosureExpression (see ast.h); its parameters get the same
    // `ref` check as any other parameter list.
    std::string interfaceName;
    std::vector<std::string> captures;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;
    bool sawInterfaceName = false;

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "identifier") == 0 && !sawInterfaceName) {
            interfaceName = getNodeText(child, source);
            sawInterfaceName = true;
        } else if (strcmp(childType, "capture_list") == 0) {
            uint32_t capCount = ts_node_child_count(child);
            for (uint32_t j = 0; j < capCount; j++) {
                TSNode capNode = ts_node_child(child, j);
                if (strcmp(ts_node_type(capNode), "identifier") == 0) {
                    captures.push_back(getNodeText(capNode, source));
                }
            }
        } else if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
            std::string pendingType;
            uint32_t paramCount = ts_node_child_count(child);
            for (uint32_t j = 0; j < paramCount; j++) {
                TSNode paramNode = ts_node_child(child, j);
                const char* pt = ts_node_type(paramNode);
                if (strcmp(pt, "type") == 0) {
                    pendingType = getNodeText(paramNode, source);
                } else if (strcmp(pt, "identifier") == 0 && !pendingType.empty()) {
                    parameters.push_back({pendingType, getNodeText(paramNode, source)});
                    pendingType.clear();
                }
            }
        } else if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            } else {
                rejectUnbuildableChild(child, source);
            }
        }
    }

    return std::make_unique<ClosureExpression>(interfaceName, std::move(captures),
                                               std::move(parameters), std::move(body));
}

StatementPtr ASTBuilder::buildIncludeStatement(TSNode node, const std::string& source) {
    std::string includeText = getNodeText(node, source);
    return std::make_unique<IncludeStatement>(includeText);
}

StatementPtr ASTBuilder::buildClassDeclaration(TSNode node, const std::string& source) {
    // child 0 is the `class` or `struct` keyword, child 1 the name
    bool isValueType = getNodeText(ts_node_child(node, 0), source) == "struct";
    TSNode name_node = ts_node_child(node, 1);
    std::string className = getNodeText(name_node, source);

    // The base name, if any, is the second `identifier` child.
    std::string baseClass;
    std::vector<StatementPtr> members;

    uint32_t childCount = ts_node_child_count(node);
    bool sawClassName = false;
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "identifier") == 0) {
            if (!sawClassName) {
                sawClassName = true;   // the class's own name
            } else if (baseClass.empty()) {
                baseClass = getNodeText(child, source);
            }
        } else if (strcmp(childType, "member_variable_declaration") == 0 ||
            strcmp(childType, "constructor_declaration") == 0 ||
            strcmp(childType, "method_declaration") == 0) {
            auto member = buildStatement(child, source);
            if (member) {
                members.push_back(std::move(member));
            } else {
                rejectUnbuildableChild(child, source);
            }
        }
    }

    auto result = std::make_unique<ClassDeclaration>(className, std::move(members));
    result->baseClass = baseClass;
    result->isValueType = isValueType;
    return result;
}

std::vector<StatementPtr> ASTBuilder::buildNamespaceDeclaration(TSNode node, const std::string& source) {
    // Returns the members as top-level statements tagged with namespaceName.
    TSNode name_node = ts_node_child(node, 1); // skip "namespace"
    std::string namespaceName = getNodeText(name_node, source);

    std::vector<StatementPtr> result;
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            // A nested namespace has no single namespaceName to tag its members with.
            if (ts_node_child_count(child) > 0 &&
                strcmp(ts_node_type(ts_node_child(child, 0)), "namespace_declaration") == 0) {
                std::ostringstream oss;
                const TSPoint p = ts_node_start_point(child);
                oss << "error[semantic/nested-namespace]: namespace `" << namespaceName
                    << "` contains a nested namespace declaration (line " << (p.row + 1)
                    << ", column " << (p.column + 1) << "). CeeLing supports only a "
                       "single level of namespace nesting -- flatten the inner "
                       "namespace's members into `" << namespaceName << "` directly, "
                       "or give it its own top-level namespace.";
                throw std::runtime_error(oss.str());
            }

            auto stmt = buildStatement(child, source);
            if (!stmt) rejectUnbuildableChild(child, source);
            if (auto cls = dynamic_cast<ClassDeclaration*>(stmt.get())) {
                cls->namespaceName = namespaceName;
            } else if (auto enm = dynamic_cast<EnumDeclaration*>(stmt.get())) {
                enm->namespaceName = namespaceName;
            } else if (auto fn = dynamic_cast<FunctionDeclaration*>(stmt.get())) {
                fn->namespaceName = namespaceName;
            }
            result.push_back(std::move(stmt));
        }
    }

    return result;
}

StatementPtr ASTBuilder::buildEnumDeclaration(TSNode node, const std::string& source) {
    TSNode name_node = ts_node_child(node, 1); // skip "enum"
    std::string enumName = getNodeText(name_node, source);

    std::vector<std::string> enumerators;
    uint32_t childCount = ts_node_child_count(node);
    bool sawEnumName = false;
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);
        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "identifier") == 0) {
            if (!sawEnumName) {
                sawEnumName = true;   // the enum's own name
            } else {
                enumerators.push_back(getNodeText(child, source));
            }
        }
    }

    return std::make_unique<EnumDeclaration>(enumName, std::move(enumerators));
}

StatementPtr ASTBuilder::buildConstructorDeclaration(TSNode node, const std::string& source) {
    TSNode name_node = ts_node_child(node, 0);
    std::string constructorName = getNodeText(name_node, source);

    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
            // Pair each `type` with the following `identifier`; a fixed stride does not
            // work because every parameter after the first has a leading ','.
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
            }
        } else if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            } else {
                rejectUnbuildableChild(child, source);
            }
        }
    }

    return std::make_unique<ConstructorDeclaration>(constructorName, std::move(parameters), std::move(body));
}

StatementPtr ASTBuilder::buildMemberVariableDeclaration(TSNode node, const std::string& source) {
    // A field has exactly 3 children (type, identifier, ";"). Extra children, such as an
    // ERROR node from `ref Node r;`, must be rejected, since only children 0 and 1 are read.
    TSNode type_node = ts_node_child(node, 0);
    TSNode identifier_node = ts_node_child(node, 1);

    std::string type = getNodeText(type_node, source);
    std::string identifier = getNodeText(identifier_node, source);

    const uint32_t childCount = ts_node_child_count(node);
    if (childCount > 3) {
        if (type == "ref") {
            // `ref Node r;` parses with `ref` as the type. A field cannot be a `ref`
            // observer because it can outlive what it observes.
            std::ostringstream oss;
            const TSPoint p = ts_node_start_point(node);
            oss << "error[semantic/forbidden-construct]: `ref` cannot be used as a "
                   "class field type (line " << (p.row + 1) << ", column "
                << (p.column + 1) << "). A field can outlive the local object a `ref` "
                   "would observe, so a stored borrow could dangle long after the "
                   "function that created it returns; declare the field as a plain "
                   "owning type instead.";
            throw std::runtime_error(oss.str());
        }
        for (uint32_t i = 3; i < childCount; ++i) {
            rejectUnbuildableChild(ts_node_child(node, i), source);
        }
    }

    // a field has no initializer, so the expression is null
    return std::make_unique<VariableDeclaration>(type, identifier, nullptr);
}

StatementPtr ASTBuilder::buildIfStatement(TSNode node, const std::string& source) {
    TSNode condition_node = ts_node_child(node, 2); // skip "if" and "("
    auto condition = buildExpression(condition_node, source);

    std::vector<StatementPtr> body;
    std::vector<StatementPtr> elseBody;

    // The `else` token splits the then-branch from the else-branch (a nested if for `else if`).
    uint32_t childCount = ts_node_child_count(node);
    bool afterElse = false;
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (getNodeText(child, source) == "else") { afterElse = true; continue; }

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        }

        const bool isStmt = strcmp(childType, "statement") == 0;
        const bool isNestedIf = strcmp(childType, "if_statement") == 0;  // `else if`
        if (!isStmt && !isNestedIf) continue;

        auto built = buildStatement(child, source);
        if (!built) rejectUnbuildableChild(child, source);
        (afterElse ? elseBody : body).push_back(std::move(built));
    }

    auto result = std::make_unique<IfStatement>(std::move(condition), std::move(body));
    result->elseBody = std::move(elseBody);
    return result;
}

StatementPtr ASTBuilder::buildWhileStatement(TSNode node, const std::string& source) {
    TSNode condition_node = ts_node_child(node, 2); // skip "while" and "("
    auto condition = buildExpression(condition_node, source);

    std::vector<StatementPtr> body;
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (!stmt) rejectUnbuildableChild(child, source);
            body.push_back(std::move(stmt));
        }
    }

    return std::make_unique<WhileStatement>(std::move(condition), std::move(body));
}

StatementPtr ASTBuilder::buildForStatement(TSNode node, const std::string& source) {
    TSNode type_node = ts_node_child(node, 2);
    TSNode identifier_node = ts_node_child(node, 3);
    TSNode container_node = ts_node_child(node, 5); // skip ":"

    std::string loopVarType = getNodeText(type_node, source);
    std::string loopVarName = getNodeText(identifier_node, source);
    auto container = buildExpression(container_node, source);

    std::vector<StatementPtr> body;
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (!stmt) rejectUnbuildableChild(child, source);
            body.push_back(std::move(stmt));
        }
    }

    return std::make_unique<ForStatement>(loopVarType, loopVarName, std::move(container),
                                          std::move(body));
}

ExpressionPtr ASTBuilder::buildMemberAccess(TSNode node, const std::string& source) {
    TSNode object_node = ts_node_child(node, 0);
    TSNode member_node = ts_node_child(node, 2); // skip "->"

    auto object = buildExpression(object_node, source);
    std::string member = getNodeText(member_node, source);

    auto access = std::make_unique<MemberAccess>(std::move(object), member);
    stampLoc(access.get(), node);   // direct callers skip buildExpression
    return access;
}

ExpressionPtr ASTBuilder::buildSubscriptExpression(TSNode node, const std::string& source) {
    TSNode object_node = ts_node_child(node, 0);
    TSNode index_node = ts_node_child(node, 2); // skip "["

    auto object = buildExpression(object_node, source);
    auto index = buildExpression(index_node, source);

    auto sub = std::make_unique<SubscriptExpression>(std::move(object), std::move(index));
    // An assignment target (`v[i] = x;`) is built by calling this directly, and the
    // bounds check reports this line.
    stampLoc(sub.get(), node);
    return sub;
}

StatementPtr ASTBuilder::buildMethodDeclaration(TSNode node, const std::string& source) {
    // A leading `<T>` and a leading `virtual` each shift the type/name child indices.
    uint32_t base = 0;
    std::string typeParameter;
    if (ts_node_child_count(node) > base &&
        strcmp(ts_node_type(ts_node_child(node, base)), "method_type_parameter") == 0) {
        TSNode tp = ts_node_child(node, base);
        // the identifier is the only named child
        if (ts_node_named_child_count(tp) > 0) {
            typeParameter = getNodeText(ts_node_named_child(tp, 0), source);
        }
        base += 1;
    }
    bool isVirtual = ts_node_child_count(node) > base &&
                     strcmp(ts_node_type(ts_node_child(node, base)), "virtual") == 0;
    if (isVirtual) base += 1;

    TSNode return_type_node = ts_node_child(node, base + 0);
    TSNode name_node = ts_node_child(node, base + 1);
    std::string returnType = getNodeText(return_type_node, source);
    std::string methodName = getNodeText(name_node, source);

    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;
    bool isConst = false;
    bool isOverride = false;
    std::string specializationArg;

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "method_specialization") == 0) {
            // the type is the only named child
            if (ts_node_named_child_count(child) > 0) {
                specializationArg = getNodeText(ts_node_named_child(child, 0), source);
            }
        } else if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
            // pair each `type` with the following `identifier`
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
            }
        } else if (strcmp(childType, "const") == 0) {
            isConst = true;
        } else if (strcmp(childType, "override") == 0) {
            isOverride = true;
        } else if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            } else {
                rejectUnbuildableChild(child, source);
            }
        }
    }

    // A primary generic method's T must be the return type (`<T> T as()`) or, when it is
    // not, appear in exactly one parameter (`<T> bool decodeInto(T out)`, which becomes
    // a mutable out-parameter, see outParamIndex_ in DanglingChecker and AstEmitter).
    // Any other use of T has no single meaning and is rejected.
    if (!typeParameter.empty()) {
        size_t paramTCount = 0;
        for (const auto& [ptype, pname] : parameters) {
            if (ptype == typeParameter) ++paramTCount;
        }
        const bool returnIsT = (returnType == typeParameter);
        if (!returnIsT && paramTCount == 0) {
            std::ostringstream oss;
            const TSPoint p = ts_node_start_point(node);
            oss << "error[semantic/unsupported-generic]: generic method `" << methodName
                << "` declares type parameter `" << typeParameter << "` but it appears in "
                   "neither the return type (`" << returnType << "`) nor any parameter (line "
                << (p.row + 1) << ", column " << (p.column + 1) << "). CeeLing supports only "
                   "return-type genericity (`<T> T " << methodName << "() {...}`) or a single "
                   "T-typed parameter (`<T> ReturnType " << methodName
                << "(T param) {...}`) -- a type parameter that appears nowhere checkable "
                   "is not supported.";
            throw std::runtime_error(oss.str());
        }
        if (paramTCount > 1) {
            std::ostringstream oss;
            const TSPoint p = ts_node_start_point(node);
            oss << "error[semantic/unsupported-generic]: generic method `" << methodName
                << "` uses its type parameter `" << typeParameter << "` for " << paramTCount
                << " different parameters (line " << (p.row + 1) << ", column "
                << (p.column + 1) << "). CeeLing supports at most one T-typed parameter.";
            throw std::runtime_error(oss.str());
        }
    }

    auto result = std::make_unique<MethodDeclaration>(returnType, methodName, std::move(parameters), std::move(body));
    result->isConst = isConst;
    result->isVirtual = isVirtual;
    result->isOverride = isOverride;
    result->typeParameter = typeParameter;
    result->specializationArg = specializationArg;
    return result;
}

// A specialization of an existing primary generic method (`<T> T as()`) written outside
// its class: `Type Class::method<Arg>(...) {...}`. Returns an ordinary MethodDeclaration
// (isExternalSpecialization set) and the class name in `targetClass`; the caller splices
// it into that class.
StatementPtr ASTBuilder::buildExternalMethodSpecialization(TSNode node, const std::string& source,
                                                            std::string& targetClass) {
    TSNode returnTypeNode = ts_node_child(node, 0);
    TSNode targetClassNode = ts_node_child(node, 1);
    // child(2) is "::"
    TSNode methodNameNode = ts_node_child(node, 3);
    std::string returnType = getNodeText(returnTypeNode, source);
    targetClass = getNodeText(targetClassNode, source);
    std::string methodName = getNodeText(methodNameNode, source);

    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<StatementPtr> body;
    std::string specializationArg;

    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 4; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "method_specialization") == 0) {
            if (ts_node_named_child_count(child) > 0) {
                specializationArg = getNodeText(ts_node_named_child(child, 0), source);
            }
        } else if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
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
            }
        } else if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            auto stmt = buildStatement(child, source);
            if (stmt) {
                body.push_back(std::move(stmt));
            } else {
                rejectUnbuildableChild(child, source);
            }
        }
    }

    if (specializationArg.empty()) {
        std::ostringstream oss;
        const TSPoint p = ts_node_start_point(node);
        oss << "error[semantic/unsupported-generic]: `" << targetClass << "::" << methodName
            << "` at line " << (p.row + 1) << ", column " << (p.column + 1)
            << " declares an external method specialization with no `<Type>` type "
               "argument -- external_method_specialization exists only to specialize an "
               "existing primary generic method for a concrete type (yaml-cpp's "
               "convert<T> customization-point shape), never to add an ordinary "
               "out-of-class method definition (rust_backend has no such syntax at all).";
        throw std::runtime_error(oss.str());
    }

    auto result = std::make_unique<MethodDeclaration>(returnType, methodName, std::move(parameters), std::move(body));
    result->specializationArg = specializationArg;
    result->isExternalSpecialization = true;
    return result;
}

void ASTBuilder::spliceExternalSpecializations(Program& program) {
    if (program.unresolvedExternalSpecializations.empty()) return;
    auto pending = std::move(program.unresolvedExternalSpecializations);
    program.unresolvedExternalSpecializations.clear();
    for (auto& [targetClass, methodDecl] : pending) {
        ClassDeclaration* target = nullptr;
        for (auto& stmt : program.statements) {
            if (auto cls = dynamic_cast<ClassDeclaration*>(stmt.get());
                cls != nullptr && classMatchesSpecializationTarget(*cls, targetClass)) {
                target = cls;
                break;
            }
        }
        if (target == nullptr) {
            std::ostringstream oss;
            oss << "error[semantic/unknown-specialization-target]: `" << targetClass << "::"
                << static_cast<MethodDeclaration*>(methodDecl.get())->name << "<"
                << static_cast<MethodDeclaration*>(methodDecl.get())->specializationArg
                << ">` at line " << methodDecl->loc.line << " specializes a method on `"
                << targetClass << "`, which is not a class declared in this program (or "
                   "merged in via #include). CeeLing cannot specialize a method on a class "
                   "it did not compile itself -- an external_method_specialization's target "
                   "must be a real, known class, the same rule validateInheritance applies "
                   "to an unknown base class.";
            throw std::runtime_error(oss.str());
        }
        target->members.push_back(std::move(methodDecl));
    }
}

} // namespace cling
