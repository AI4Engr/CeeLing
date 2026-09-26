#include "ast_builder.h"
#include "generator/library_summary.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace cling {

namespace {

// If `snippet` contains a construct that CeeLing deliberately forbids (Phase 1 escape
// hatches, CLAUDE.md threat model #13-18), return an explanatory suggestion naming
// *why* it's forbidden. These constructs have no grammar production, so they always
// surface as an unbuildable node -- this turns that into a legible, intentional
// rejection instead of a bare "syntax error".
std::string forbiddenConstructSuggestion(const std::string& snippet) {
    if (snippet.find("reinterpret_cast") != std::string::npos ||
        snippet.find("const_cast") != std::string::npos ||
        snippet.find("static_cast") != std::string::npos) {
        return " Suggestion: C++-style casts are forbidden in CeeLing (CLAUDE.md hatch "
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
    // Note: `T* name` is grammatically indistinguishable from multiplication `a * b`
    // (CeeLing has no separate pointer-declarator syntax), so it is NOT handled here
    // as a parse-error pattern -- `EscapeHatchChecker` catches it semantically instead
    // (a `ClassName * identifier` expression statement, where the left operand is a
    // known class, cannot be a real multiplication and must be an attempted raw
    // pointer declaration).
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

// A body-collecting loop hit a child that either is an ERROR/missing node, or is a
// "statement" node that failed to build. Both mean the source contains something
// CeeLing cannot express -- raise it as a real error instead of silently dropping the
// statement (which would let unsafe/unsupported constructs pass through unseen by the
// dangling checker and the escape-hatch checker).
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

// Stamp a node's source position from its originating CST node (1-based).
static void stampLoc(ASTNode* astNode, TSNode tsNode) {
    if (astNode == nullptr) return;
    const TSPoint p = ts_node_start_point(tsNode);
    astNode->loc.line = static_cast<int>(p.row) + 1;
    astNode->loc.column = static_cast<int>(p.column) + 1;
}

// Does `cls` match an external_method_specialization's target class name,
// which may be spelled bare (`Node`) or namespace-qualified
// (`YAML::Node`)? Mirrors how every other pass in this codebase (e.g.
// AstEmitter::collectClassNames) dual-registers a namespaced class under
// both spellings, rather than requiring the specialization to be written
// with the SAME qualification the class declaration happened to use.
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
    bool hasStructuralAnchor = false;
    bool hasUnresolvedTopLevel = false;
    TSNode firstUnresolvedNode{};
    // (targetClassName, parsedMethodDeclaration) for every top-level
    // external_method_specialization seen this pass -- spliced into their
    // target ClassDeclaration's `members` AFTER every top-level statement
    // has been built (see the splice loop after this function's main loop),
    // so declaration order relative to the target class never matters.
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

        // A top-level "//" comment is declared in the grammar's `extras` (skippable
        // anywhere, like whitespace -- see grammar.js) and reaches here as a bare
        // `comment`-typed child of source_file, distinct from the `statement` wrapper
        // every real declaration is wrapped in. buildNamespaceDeclaration's own
        // member loop already ignores anything that isn't `statement`/`ERROR` for
        // exactly this reason; this top-level loop lacked the same guard, so a bare
        // comment fell through to buildStatement (which doesn't recognize a raw
        // `comment` node), returned null, and got recorded as an "unresolved"
        // top-level statement -- wrongly rejecting any file whose ONLY top-level
        // content besides the comment was a namespace containing solely free
        // functions (no class/MainFunction to satisfy hasStructuralAnchor below),
        // with a misleading error pointing at the comment itself. Skip it instead.
        if (strcmp(ts_node_type(child), "comment") == 0) {
            continue;
        }

        // `namespace X { ... }` is handled specially, not via buildStatement: it
        // desugars into MULTIPLE top-level statements (each member's own
        // ClassDeclaration/EnumDeclaration, tagged with namespaceName), which does
        // not fit buildStatement's one-node-in/one-statement-out contract. It is
        // deliberately absent from buildStatement's own dispatch table for this
        // reason -- only top-level namespace declarations are supported (the
        // grammar's namespace_declaration is reachable from $.statement generally,
        // but a NESTED occurrence reaching here would mean it appeared inside a
        // class/function body, which buildStatement would silently skip; that's
        // caught by hasUnresolvedTopLevel/hasStructuralAnchor same as any other
        // unhandled node).
        // Top-level children are wrapped in a generic "statement" node (per the
        // grammar's `source_file: repeat($.statement)`), so unwrap one level before
        // checking for namespace_declaration specifically -- buildStatement does the
        // same unwrap internally for every OTHER node type via its own "statement"
        // passthrough case, but namespace needs the raw inner node before dispatch.
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

        // `ReturnType ClassName::methodName<Type>(...) {...}` at top level
        // (yaml-cpp's convert<T> customization-point shape) doesn't fit
        // buildStatement's one-node-in/one-statement-out contract either --
        // it produces a MethodDeclaration that must be SPLICED into an
        // EXISTING class's own `members` vector (possibly declared earlier
        // OR later in this same file, or merged in from a separate .clg via
        // #include -- see mergeExternalDeclarations in main.cpp, which runs
        // BEFORE this builder never sees it; the splice here only needs to
        // handle same-file ordering, since an external class's
        // ClassDeclaration is already a real top-level statement by the
        // time ITS .clg is built standalone). Collected here and spliced in
        // a second pass below (after every top-level statement, including
        // every ClassDeclaration, has been built) so declaration ORDER
        // relative to the target class never matters.
        if (strcmp(ts_node_type(inner), "external_method_specialization") == 0) {
            std::string targetClass;
            auto methodDecl = buildExternalMethodSpecialization(inner, source, targetClass);
            stampLoc(methodDecl.get(), inner);
            // Raw pointer captured BEFORE ownership moves into
            // pendingExternalSpecializations (and from there into the target
            // class's `members` in the splice pass below) -- the marker
            // below is what AstEmitter actually walks for emission position;
            // the splice destination only matters to the checkers.
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

    // Splice every external_method_specialization into its target class's
    // OWN members vector now, after every top-level statement (including
    // every ClassDeclaration) has been built -- this is what lets every
    // downstream pass (EscapeHatchChecker, SmartPointerAnalyzer, AstEmitter,
    // DanglingChecker) see it via their EXISTING "walk cls->members looking
    // for MethodDeclaration" logic, with zero changes to any of them beyond
    // whatever needs to specifically branch on isExternalSpecialization
    // (AstEmitter, to emit it at namespace scope -- see that flag's own
    // comment in ast.h). A target NOT found among THIS file's own statements
    // is deferred to program->unresolvedExternalSpecializations rather than
    // treated as an error here -- it may still be a real, known class
    // declared in a DIFFERENT .clg file reached via #include, which
    // mergeExternalDeclarations (main.cpp) only resolves AFTER this function
    // returns (confirmed via probe: the cross-file case genuinely can't be
    // resolved at this point in the pipeline). main.cpp calls
    // spliceExternalSpecializations again post-merge, where "still not
    // found" really does mean unknown and is a hard error (CLAUDE.md's
    // "default-reject, never default-allow", same as validateInheritance's
    // unknown-base-class rule below) -- a specialization that silently
    // vanished would be far more confusing than a clear error.
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

// A base clause must name a class this program actually declares, and the
// inheritance graph must be acyclic. Both are correctness prerequisites, not just
// politeness: every analysis module flattens inherited members at collection time by
// walking base-before-derived, and an unknown base (nothing to copy from) or a cycle
// (`A : B`, `B : A` -- no valid order exists) would otherwise silently produce a
// wrong flattened view or loop forever.
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

        // Walk up the chain from this class; revisiting `name` means a cycle.
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
    } else if (strcmp(node_type, "throw_statement") == 0) {
        result = buildThrowStatement(node, source);
    } else if (strcmp(node_type, "try_statement") == 0) {
        result = buildTryStatement(node, source);
    } else if (strcmp(node_type, "container_declaration") == 0) {
        result = buildContainerDeclaration(node, source);
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

StatementPtr ASTBuilder::buildContainerDeclaration(TSNode node, const std::string& source) {
    // container_declaration: generic_type identifier ";"
    TSNode type_node = ts_node_child(node, 0);
    TSNode identifier_node = ts_node_child(node, 1);

    // `std::vector<ref Node>` (a container-held observer): the grammar has no
    // `ref`-qualified type-argument production, so `ref` inside the `< >` is left as
    // an ERROR node nested under generic_type rather than a sibling of this whole
    // declaration -- ts_node_has_error walks the full subtree, unlike the sibling-
    // count check buildMemberVariableDeclaration uses for its own `ref`-misparse case
    // (there the ERROR is a direct sibling, not nested). Container-held observers are
    // forbidden by construction the same way `ref` fields are (#3-adjacent: an
    // observer stored anonymously inside a reallocatable container has no reliable
    // per-element provenance), so give a targeted diagnostic instead of the generic
    // parse-error fallback.
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

    // `std::map<K,V>` (Phase 2.6 map slice) is restricted to primitive-typed keys and
    // values this slice: a map's element-ownership question (unique_ptr vs
    // shared_ptr per entry, keyed on usage across every insertion site) is exactly
    // the problem vector slice 1 solved for push_back, and re-solving it for map
    // isn't worth it until a real class-valued-map need shows up. Primitives
    // sidestep the question entirely (always plain-copy values), so this is a
    // conservative reject of anything else -- including class names, which this
    // builder cannot even distinguish from a typo, so any non-primitive value is
    // rejected the same way, per CLAUDE.md's "ambiguous -> conservative reject" rule.
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

    if (strcmp(ts_node_type(lhs_node), "subscript_expression") == 0) {
        // Subscript target `m[key] = value;` (std::map insert-or-update, Phase 2.6 map
        // slice). `object` is always a bare container identifier here -- the grammar's
        // subscript_expression production allows a chain, but map/vector locals are
        // never themselves the result of a further subscript/member access -- so the
        // root name is just object's identifier, no walk needed (unlike member_access,
        // where `a.b.c = ...` can nest).
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
        // primary_expression: scoped_identifier | identifier | number |
        // string_literal | char_literal | '(' expression ')'. Every alternative
        // except the last has exactly ONE child, so `child(0)` used to be
        // correct -- but the grouped `(expr)` form has THREE children ('(',
        // expression, ')'), and child(0) is the LITERAL '(' TOKEN, not the
        // inner expression. Using child(0) unconditionally silently built
        // garbage (or crashed downstream) for any parenthesized sub-expression
        // (`bool b = (a);`, `a && (b || c)`) -- confirmed via a direct probe
        // that this failed to parse at all before this fix. Fixed by finding
        // the NAMED child (skips literal tokens like '(' / ')' automatically),
        // which is correct for every alternative including the grouped form.
        if (ts_node_named_child_count(node) > 0) {
            TSNode child = ts_node_named_child(node, 0);
            return buildExpression(child, source);
        }
    } else if (strcmp(node_type, "expression") == 0) {
        // If we get a generic "expression" node, look at its first child
        if (ts_node_child_count(node) > 0) {
            TSNode child = ts_node_child(node, 0);
            return buildExpression(child, source);
        }
    }

    // An unrecognized/ERROR node, or a recognized builder that itself failed (e.g. a
    // binary_expression whose operand couldn't be built): never return null silently.
    // Every call site treats the result as non-null; a silent null here was a latent
    // crash (and, for constructs like `reinterpret_cast<T>(x)` that mis-parse as
    // ordinary comparisons rather than an ERROR node, a silent way for a forbidden
    // construct's operand to vanish from the emitted C++ instead of being rejected).
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
    // scoped_identifier: identifier ("::" identifier)+ — keep the whole `std::cout` text.
    std::string name = getNodeText(node, source);
    return std::make_unique<ScopedIdentifier>(name);
}

ExpressionPtr ASTBuilder::buildStringLiteral(TSNode node, const std::string& source) {
    // string_literal: a double-quoted token; keep it verbatim (quotes included).
    std::string text = getNodeText(node, source);
    return std::make_unique<StringLiteral>(text);
}

ExpressionPtr ASTBuilder::buildCharLiteral(TSNode node, const std::string& source) {
    // char_literal: a single-quoted token; keep it verbatim (quotes included) --
    // CeeLing's `'x'` and C++'s `'x'` char-literal syntax are identical.
    std::string text = getNodeText(node, source);
    return std::make_unique<CharLiteral>(text);
}

ExpressionPtr ASTBuilder::buildUnaryExpression(TSNode node, const std::string& source) {
    // unary_expression: operator operand. The operator token ('!', '-', '*', or
    // '++') is inlined, so the only named child is the operand; recover the
    // operator from the leading text. '++' is TWO characters -- unlike the other
    // three single-character operators, a bare `full[0]` would silently truncate
    // it to a single '+' (a real bug this fixes, not present before '++' existed).
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
    // ext_fn_declaration: 'ext_fn' type identifier "(" optional(parameter_list) ")"
    //                     optional('borrows' number) ";"
    // Unlike function_declaration, this production's FIRST child is the leading
    // 'ext_fn' keyword itself (an anonymous token, but ts_node_child (unlike the
    // tree-sitter CLI's pretty-printer, which hides anonymous nodes by default)
    // still counts it) -- so type/name can't be read by fixed numeric index the
    // way function_declaration's builder does. Matched by type name while
    // iterating instead, the same way buildClosureExpression's own leading
    // 'closure' keyword is handled.
    std::string returnType;
    std::string funcName;
    bool sawReturnType = false;
    std::vector<std::pair<std::string, std::string>> parameters;
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
        } else if (strcmp(childType, "number") == 0) {
            // The optional `borrows N` clause's N -- the only number that can ever
            // appear as a direct child of this production (parameter default
            // values don't exist in this grammar), so no further disambiguation
            // is needed.
            borrowsParam = std::stoi(getNodeText(child, source));
        } else if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        }
    }

    // v1 scope boundary (see TODO.md/CLAUDE.md's ext_fn design record): an
    // external function's parameters/return must be primitive or
    // std::vector<primitive> -- never a bare class name. Object-typed ext_fn
    // signatures need a real hand-written borrow-summary syntax beyond a
    // single `borrows N` return clause (e.g. which param a NON-return
    // parameter borrows from another parameter), which does not exist yet.
    // Conservative reject here, at the parse boundary, rather than letting an
    // unsupported shape silently reach the checkers below and be mishandled.
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
               "designed yet (see TODO.md's ext_fn entry).";
        throw std::runtime_error(oss.str());
    };
    rejectUnsupportedType(returnType, "return");
    for (const auto& [paramType, paramName] : parameters) {
        rejectUnsupportedType(paramType, "parameter `" + paramName + "`");
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

    return std::make_unique<ExternFunctionDeclaration>(returnType, funcName,
                                                        std::move(parameters), borrowsParam);
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

ExpressionPtr ASTBuilder::buildTemplateCall(TSNode node, const std::string& source) {
    // template_call: (identifier | member_access | subscript_expression) "<" type ">"
    //                "(" optional(argument_list) ")"
    // Reuses FunctionCall (templateArg set) rather than a separate AST node --
    // every analysis module's existing FunctionCall handling (move-tracking,
    // isKnownCallable, summaries, emission) applies unchanged once templateArg is
    // consulted to pick the right lookup key (specializationKey), so a distinct
    // node type would only duplicate that dispatch surface for no benefit.
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
        // Bare `f<int>(...)`, or `v[i]<int>(...)` (subscript callee) -- the same
        // free-call shape function_call already supports for a non-method callee.
        std::string funcName = getNodeText(calleeNode, source);
        auto fc = std::make_unique<FunctionCall>(funcName, std::move(arguments));
        fc->templateArg = templateArg;
        result = std::move(fc);
    }
    return result;
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
    // return_statement: "return" optional(expression) ";" -- a bare `return;`
    // (void early-exit) has NO named child at all (only the unnamed literal
    // tokens "return"/";"), so ReturnStatement::expression stays null rather
    // than being built from whatever ts_node_child(node, 1) happened to be
    // (previously the semicolon token itself when no expression was
    // written, before the grammar made this slot genuinely optional).
    if (ts_node_named_child_count(node) == 0) {
        return std::make_unique<ReturnStatement>(nullptr);
    }
    TSNode expression_node = ts_node_named_child(node, 0);
    auto expr = buildExpression(expression_node, source);
    return std::make_unique<ReturnStatement>(std::move(expr));
}

StatementPtr ASTBuilder::buildThrowStatement(TSNode node, const std::string& source) {
    // throw_statement: "throw" expression ";"
    TSNode expression_node = ts_node_child(node, 1); // Skip "throw"
    auto expr = buildExpression(expression_node, source);
    return std::make_unique<ThrowStatement>(std::move(expr));
}

StatementPtr ASTBuilder::buildTryStatement(TSNode node, const std::string& source) {
    // try_statement: "try" "{" stmts "}" "catch" "(" type identifier ")" "{" stmts "}"
    // repeat($.statement) produces no wrapper node, so try-body and catch-body
    // "statement" children sit as flat siblings of this node -- split them on the
    // "catch" keyword the same way buildIfStatement splits then/else on "else".
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
    // main_function: type "main" "(" ")" "{" repeat(statement) "}"
    TSNode type_node = ts_node_child(node, 0);
    std::string returnType = getNodeText(type_node, source);

    std::vector<StatementPtr> body;

    // Parse body statements
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
    // new_expression: "new" (identifier | scoped_identifier) "(" optional(argument_list) ")"
    // getNodeText captures either shape verbatim, so `new YAML::Node(...)` naturally
    // yields className="YAML::Node" with no extra handling here -- callers that need
    // just the simple name (e.g. AstEmitter::smartPointerType, which asks
    // SmartPointerAnalyzer -- namespace-unaware) strip the qualifier themselves.
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

ExpressionPtr ASTBuilder::buildClosureExpression(TSNode node, const std::string& source) {
    // closure_expression: "closure" identifier "[" optional(capture_list) "]"
    //                     "(" optional(parameter_list) ")" "{" repeat(statement) "}"
    // Builds a TRANSIENT ClosureExpression (see its own doc comment in ast.h) --
    // ASTBuilder::desugarClosures replaces every one of these with a
    // NewExpression before any checker ever runs. Parameters go through the
    // SAME rejectObserverParams check every other parameter list uses (`ref`
    // in a closure's own parameter list is exactly as forbidden as anywhere
    // else -- see the 4 existing call sites this is the 5th of).
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
                // Skip comma tokens.
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
        // Skip "closure", "[", "]", "(", ")", "{", "}" literal tokens.
    }

    return std::make_unique<ClosureExpression>(interfaceName, std::move(captures),
                                               std::move(parameters), std::move(body));
}

StatementPtr ASTBuilder::buildIncludeStatement(TSNode node, const std::string& source) {
    // include_statement: "#include" choice(seq('<', /[^>]+/, '>'), seq('"', /[^"]+/, '"'))
    std::string includeText = getNodeText(node, source);
    return std::make_unique<IncludeStatement>(includeText);
}

StatementPtr ASTBuilder::buildClassDeclaration(TSNode node, const std::string& source) {
    // class_declaration: choice("class", "struct") identifier
    //                    optional(":" "public" identifier)
    //                    "{" "public" ":" repeat(member) "}" optional(";")
    // `choice('class', 'struct')` still occupies exactly one child slot at
    // index 0 regardless of which keyword matched, so the existing "skip
    // child 0, name is child 1" indexing below is UNCHANGED -- only the new
    // isValueType read (child 0's own text) is added.
    bool isValueType = getNodeText(ts_node_child(node, 0), source) == "struct";
    TSNode name_node = ts_node_child(node, 1); // Skip "class"/"struct"
    std::string className = getNodeText(name_node, source);

    // With a base clause, the base name is a SECOND `identifier` child sitting
    // directly under class_declaration (the `:` / `public` tokens are anonymous and
    // carry no node). Without one, there is exactly one identifier child. So "the
    // second identifier child, if present" is the base -- no field name needed.
    std::string baseClass;
    std::vector<StatementPtr> members;

    // Parse all children to find class members
    uint32_t childCount = ts_node_child_count(node);
    bool sawClassName = false;
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "identifier") == 0) {
            if (!sawClassName) {
                sawClassName = true;   // the class's own name, already captured above
            } else if (baseClass.empty()) {
                baseClass = getNodeText(child, source);
            }
        } else if (strcmp(childType, "member_variable_declaration") == 0 ||
            strcmp(childType, "constructor_declaration") == 0 ||
            strcmp(childType, "method_declaration") == 0) {
            // Direct member
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
    // namespace_declaration: "namespace" identifier "{" repeat(statement) "}"
    // Desugars to a flat list of top-level statements, each tagged with
    // namespaceName -- see the ClassDeclaration/EnumDeclaration comments on that
    // field for why (every analysis module keeps seeing ordinary top-level
    // declarations; only AstEmitter reads namespaceName, to re-wrap on output).
    TSNode name_node = ts_node_child(node, 1); // Skip "namespace"
    std::string namespaceName = getNodeText(name_node, source);

    std::vector<StatementPtr> result;
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "ERROR") == 0 || ts_node_is_missing(child)) {
            rejectUnbuildableChild(child, source);
        } else if (strcmp(childType, "statement") == 0) {
            // A nested namespace_declaration would show up here as `statement`'s
            // sole child -- reject it with a targeted diagnostic instead of
            // silently mis-flattening (a namespace inside a namespace has no
            // single unambiguous namespaceName to tag its members with).
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
    // enum_declaration: "enum" identifier "{" identifier (',' identifier)* ","? "}" ";"?
    TSNode name_node = ts_node_child(node, 1); // Skip "enum"
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
                sawEnumName = true;   // the enum's own name, already captured above
            } else {
                enumerators.push_back(getNodeText(child, source));
            }
        }
    }

    return std::make_unique<EnumDeclaration>(enumName, std::move(enumerators));
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
            // Pair each `type` node with the following `identifier` node. A
            // fixed j+=2 stride is WRONG here: parameter_list's grammar is
            // seq(type, identifier), repeat(seq(',', type, identifier)) -- the
            // FIRST parameter is 2 children wide, but every parameter after it
            // has a leading ',' sibling too, making it 3 children wide. A fixed
            // stride desyncs after the first parameter and silently drops
            // every parameter after it (confirmed: `Token(int k, string v)`
            // compiled a constructor taking only `k`, `v` vanished with no
            // error). Walk every child and pair by TYPE instead, matching
            // buildFunctionDeclaration's already-correct pattern.
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
    // member_variable_declaration: type identifier ";"
    // A well-formed field is exactly 3 children (type, identifier, ";"). Anything
    // beyond that (e.g. a trailing ERROR node from a misparse like `ref Node r;` --
    // the grammar has no `ref`-qualified field, so `ref` parses as the type and `r`
    // is left over as an ERROR sibling) must be rejected, not silently ignored: this
    // builder only ever reads children 0 and 1, so an extra child would otherwise
    // vanish from the tree exactly like the Phase 1 silent-drop bug.
    TSNode type_node = ts_node_child(node, 0);
    TSNode identifier_node = ts_node_child(node, 1);

    std::string type = getNodeText(type_node, source);
    std::string identifier = getNodeText(identifier_node, source);

    const uint32_t childCount = ts_node_child_count(node);
    if (childCount > 3) {
        if (type == "ref") {
            // `ref Node r;` as a field: the grammar has no `ref`-qualified field
            // production, so `ref` itself parses as the type and the real field name
            // is left dangling as an ERROR sibling. Give a targeted message instead of
            // the generic parse-error fallback -- this is threat #3 (reference-stored-
            // in-field escape): a field can never be a `ref` observer, because a field
            // can outlive the local it would be observing.
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
    // while_statement: "while" "(" expression ")" "{" repeat(statement) "}"
    TSNode condition_node = ts_node_child(node, 2); // Skip "while" and "("
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
    // for_statement: "for" "(" type identifier ":" expression ")" "{" repeat(statement) "}"
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
    // member_access: primary_expression "->" identifier
    TSNode object_node = ts_node_child(node, 0);
    TSNode member_node = ts_node_child(node, 2); // Skip "->"

    auto object = buildExpression(object_node, source);
    std::string member = getNodeText(member_node, source);

    return std::make_unique<MemberAccess>(std::move(object), member);
}

ExpressionPtr ASTBuilder::buildSubscriptExpression(TSNode node, const std::string& source) {
    // subscript_expression: expression "[" expression "]"
    TSNode object_node = ts_node_child(node, 0);
    TSNode index_node = ts_node_child(node, 2); // Skip "["

    auto object = buildExpression(object_node, source);
    auto index = buildExpression(index_node, source);

    return std::make_unique<SubscriptExpression>(std::move(object), std::move(index));
}

StatementPtr ASTBuilder::buildMethodDeclaration(TSNode node, const std::string& source) {
    // method_declaration: optional(method_type_parameter) optional("virtual") type
    //                     identifier optional(method_specialization) "("
    //                     optional(parameter_list) ")" optional("const")
    //                     optional("override") "{" repeat(statement) "}"
    //
    // Both a leading `<T>` and a leading `virtual` shift the type/name child
    // indices, so scan for named markers rather than assuming fixed positions --
    // more robust than the previous single-flag/fixed-offset approach now that TWO
    // independent optional prefixes can each shift the type/identifier pair.
    uint32_t base = 0;
    std::string typeParameter;
    if (ts_node_child_count(node) > base &&
        strcmp(ts_node_type(ts_node_child(node, base)), "method_type_parameter") == 0) {
        TSNode tp = ts_node_child(node, base);
        // method_type_parameter: "<" identifier ">" -- the identifier is its only
        // named child.
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

    // Parse all children to find parameters and body
    uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(node, i);
        const char* childType = ts_node_type(child);

        if (strcmp(childType, "method_specialization") == 0) {
            // method_specialization: "<" type ">" -- the type is its only named child.
            if (ts_node_named_child_count(child) > 0) {
                specializationArg = getNodeText(ts_node_named_child(child, 0), source);
            }
        } else if (strcmp(childType, "parameter_list") == 0) {
            rejectObserverParams(child);
            // Pair each `type` node with the following `identifier` node --
            // NOT a fixed j+=2 stride (see buildConstructorDeclaration's
            // identical fix for why that silently drops every parameter after
            // the first: only the FIRST parameter is 2 children wide, every
            // one after it has a leading ',' sibling making it 3 wide).
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

    // A primary generic method's type parameter T must appear in the return
    // type (`<T> T as()`, CeeLing's original return-type-genericity slice --
    // T MAY also appear in a parameter here, e.g. the pre-existing `<T> T
    // as(T fallback) {...}` pattern: an ordinary by-value parameter, no
    // special lowering needed, since T isn't a real class type and already
    // falls through to a bare, verbatim emission), OR, when T is NOT the
    // return type, in exactly one parameter's type (`<T> bool decodeInto(T
    // out)`, added for yaml-cpp's convert<T>::decode out-param shape -- see
    // CLAUDE.md/TODO.md; that one parameter becomes a mutable out-parameter,
    // see DanglingChecker::outParamIndex_/AstEmitter::outParamIndex_). T is
    // never checked as a real generic type anywhere else in the pipeline
    // (EscapeHatchChecker only tracks callable names/arities, never param
    // types; SmartPointerAnalyzer/AstEmitter never inspect a PRIMARY's body
    // deeply since it's always a trivial stub -- only a concrete
    // SPECIALIZATION's body, with a real concrete type substituted for T, is
    // ever emitted/analyzed for real), so both positions are equally
    // supportable with no further core changes -- but T appearing in
    // NEITHER the return type nor any parameter, or in more than one
    // parameter when it's not also the return type, has no single,
    // unambiguous meaning and is rejected per CLAUDE.md's "ambiguous ->
    // conservative reject" rule.
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

// external_method_specialization: type identifier "::" identifier
//                                  method_specialization "(" optional(parameter_list) ")"
//                                  "{" repeat(statement) "}"
//
// yaml-cpp's convert<T> customization-point shape: a library consumer
// specializes an EXISTING primary generic method (`<T> T as()`, declared
// inside `targetClass`'s own class body, possibly in a different .clg
// file) for their own type, from OUTSIDE that class. Returns an ordinary
// MethodDeclaration (isExternalSpecialization=true) plus the parsed
// target class name via the out-parameter `targetClass` -- the caller
// (buildProgram) splices it into that class's ClassDeclaration::members,
// so every downstream pass that already walks `members` looking for
// MethodDeclarations picks it up automatically (see ast.h's
// isExternalSpecialization comment for the full rationale).
StatementPtr ASTBuilder::buildExternalMethodSpecialization(TSNode node, const std::string& source,
                                                            std::string& targetClass) {
    TSNode returnTypeNode = ts_node_child(node, 0);
    TSNode targetClassNode = ts_node_child(node, 1);
    // child(2) is the literal "::" token.
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
