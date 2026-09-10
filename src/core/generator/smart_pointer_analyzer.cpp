#include "smart_pointer_analyzer.h"

#include "class_hierarchy.h"
#include "library_summary.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>

namespace cling {

namespace {

constexpr const char* kGlobalScope = "__global__";

std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

int countChar(const std::string& text, char needle) {
    return static_cast<int>(std::count(text.begin(), text.end(), needle));
}

std::string makeMemberKey(const std::string& className, const std::string& memberName) {
    return className + "." + memberName;
}

std::string makeSymbolKey(const std::string& scopeName, const std::string& symbolName) {
    return scopeName + "|" + symbolName;
}

std::string pointerTypeName(PointerType type) {
    switch (type) {
        case PointerType::UNIQUE:
            return "std::unique_ptr";
        case PointerType::WEAK:
            return "std::weak_ptr";
        default:
            return "std::shared_ptr";
    }
}

bool isBackReferenceName(const std::string& memberName) {
    return memberName == "prev" || memberName == "previous" ||
           memberName == "parent" || memberName == "back" ||
           memberName == "owner";
}

struct ParsedValue {
    bool isNullptr = false;
    bool isNewExpression = false;
    bool isIdentifier = false;
    bool isMemberAccess = false;
    std::string identifier;
    std::string objectName;
    std::string memberName;
    std::string newClassName;
};

ParsedValue parseValue(const std::string& rawText) {
    ParsedValue parsed;
    const std::string text = trim(rawText);
    std::smatch match;

    if (text == "nullptr") {
        parsed.isNullptr = true;
        return parsed;
    }

    if (std::regex_match(text, match, std::regex(R"(^new\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"))) {
        parsed.isNewExpression = true;
        parsed.newClassName = match[1].str();
        return parsed;
    }

    if (std::regex_match(text, match, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)$)"))) {
        parsed.isIdentifier = true;
        parsed.identifier = match[1].str();
        return parsed;
    }

    if (std::regex_match(text, match, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*(?:\.|->)\s*([A-Za-z_][A-Za-z0-9_]*)$)"))) {
        parsed.isMemberAccess = true;
        parsed.objectName = match[1].str();
        parsed.memberName = match[2].str();
        return parsed;
    }

    return parsed;
}

PointerType parsePointerType(const std::string& rawType) {
    const std::string type = trim(rawType);
    if (type == "unique" || type == "unique_ptr") {
        return PointerType::UNIQUE;
    }
    if (type == "weak" || type == "weak_ptr") {
        return PointerType::WEAK;
    }
    return PointerType::SHARED;
}

std::string chooseMethodLikeScope(const std::string& currentClass, const std::string& currentFunction) {
    if (!currentClass.empty() && !currentFunction.empty()) {
        return currentClass + "::" + currentFunction;
    }
    if (!currentClass.empty()) {
        return currentClass;
    }
    if (!currentFunction.empty()) {
        return currentFunction;
    }
    return kGlobalScope;
}

} // namespace

void SmartPointerAnalyzer::analyzeProgram(const Program& program, const std::string& source) {
    variableReferences.clear();
    classRelationships.clear();
    pointerDecisions.clear();
    pointerReasons.clear();
    classNames.clear();
    explicitHints.clear();
    explicitHintLines.clear();
    classesNeedingSharedRoots.clear();
    bidirectionalOwnerMembers.clear();
    bidirectionalBackMembers.clear();
    symbolTypesByScope.clear();
    bidirectionalAssignmentSeeds.clear();
    diagnostics.clear();
    reportLines.clear();

    containerElementShared_.clear();
    classMembersByClass_.clear();
    methodReturnKinds_.clear();
    classesNeedingSharedFromThis_.clear();
    valueTypeClasses_.clear();
    readOnlyFreeFunctionParams_.clear();

    // Independent of everything else below -- reads only the raw Program, writes
    // only valueTypeClasses_/readOnlyFreeFunctionParams_ -- so they can run
    // first with no ordering dependency.
    classifyValueTypeClasses(program);
    classifyReadOnlyFreeFunctionParams(program);

    firstPassCollectReferences(program);
    if (!source.empty()) {
        scanSource(source);
    }
    // Runs BEFORE secondPassDeterminePointerTypes: a field this pass decides
    // needs SHARED (because some method returns it as an alias) writes into
    // explicitHints, which secondPass's applyExplicitHints call then picks up
    // through the exact same lookup path a real //@ptr hint would use --
    // ownership is INFERRED here, never user-annotated (CLAUDE.md), but reuses
    // the hint mechanism's plumbing rather than adding a parallel one.
    inferAliasReturningMethods(program);
    secondPassDeterminePointerTypes();

    // Container element-kind scan runs AFTER the two-pass decision above, since it
    // asks "what pointer kind did each push_back argument's OWN variable already
    // get decided as" -- e.g. `v.push_back(a);` needs `a`'s pointer kind (decided
    // from a's own non-container usages) to know whether this push forces v's
    // element kind to shared_ptr.
    scanContainerElementKinds(program);

    // Parameter-storage promotion runs after the two-pass decision (needs a
    // field's own already-decided pointer kind to know whether storing a param
    // into it forces the param to be shared), and call-site propagation runs
    // after THAT (needs to know which parameters sub-pass 1 just promoted). See
    // Chapter 18 for why this is a genuinely new third pass rather than an
    // extension of the existing two.
    promoteParamsStoredInSharedFields(program);
    promoteSharedCallArguments(program);
    // Must run LAST: needs every local's FINAL pointer-kind decision,
    // including whatever promoteSharedCallArguments (immediately above) just
    // changed.
    promoteReturnsOfSharedLocals(program);
    // Genuinely last: needs every CLASS METHOD's final alias-return
    // classification (including whatever promoteReturnsOfSharedLocals just
    // set) to correctly classify a free function that calls one.
    inferAliasReturningFreeFunctions(program);
}

PointerType SmartPointerAnalyzer::getPointerType(const std::string& className, const std::string& variableName) const {
    const std::string suffix = "|" + className + "." + variableName;
    bool found = false;
    PointerType resolved = PointerType::SHARED;

    for (const auto& [key, decision] : pointerDecisions) {
        if (key.size() >= suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (!found) {
                resolved = decision;
                found = true;
            } else if (resolved != decision) {
                return PointerType::SHARED;
            }
        }
    }

    if (found) {
        return resolved;
    }

    if (classRequiresSharedRoot(className)) {
        return PointerType::SHARED;
    }

    return PointerType::SHARED;
}

PointerType SmartPointerAnalyzer::getPointerTypeForVariable(const std::string& variableName) const {
    const std::string suffix = "." + variableName;
    bool found = false;
    PointerType resolved = PointerType::SHARED;

    for (const auto& [key, decision] : pointerDecisions) {
        if (key.size() >= suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (!found) {
                resolved = decision;
                found = true;
            } else if (resolved != decision) {
                return PointerType::SHARED;
            }
        }
    }

    return found ? resolved : PointerType::SHARED;
}

PointerType SmartPointerAnalyzer::getPointerTypeForVariableInScope(
    const std::string& scopeName, const std::string& variableName) const {
    const std::string prefix = normalizeScope(scopeName) + "|";
    const std::string suffix = "." + variableName;
    bool found = false;
    PointerType resolved = PointerType::SHARED;

    for (const auto& [key, decision] : pointerDecisions) {
        if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) continue;
        if (key.size() < suffix.size() ||
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        if (!found) {
            resolved = decision;
            found = true;
        } else if (resolved != decision) {
            return PointerType::SHARED;
        }
    }

    return found ? resolved : PointerType::UNIQUE;
}

std::string SmartPointerAnalyzer::buildAnalysisReport() const {
    std::ostringstream oss;
    oss << "CeeLing pointer analysis report\n";

    if (reportLines.empty()) {
        oss << "- No class pointer decisions were recorded.\n";
    } else {
        for (const auto& line : reportLines) {
            oss << "- " << line << "\n";
        }
    }

    if (!diagnostics.empty()) {
        oss << "Warnings\n";
        for (const auto& diagnostic : diagnostics) {
            oss << "- line " << diagnostic.line << ": " << diagnostic.message;
            if (!diagnostic.suggestion.empty()) {
                oss << " Suggestion: " << diagnostic.suggestion;
            }
            oss << "\n";
        }
    }

    return oss.str();
}

const std::vector<AnalysisDiagnostic>& SmartPointerAnalyzer::getDiagnostics() const {
    return diagnostics;
}

bool SmartPointerAnalyzer::classRequiresSharedRoot(const std::string& className) const {
    return classesNeedingSharedRoots.find(className) != classesNeedingSharedRoots.end();
}

bool SmartPointerAnalyzer::containerElementIsShared(const std::string& containerVarName) const {
    auto it = containerElementShared_.find(containerVarName);
    return it != containerElementShared_.end() && it->second;
}

PointerType SmartPointerAnalyzer::getMethodReturnPointerType(const std::string& className,
                                                              const std::string& methodName) const {
    auto it = methodReturnKinds_.find(makeMemberKey(className, methodName));
    return it != methodReturnKinds_.end() ? it->second : PointerType::UNIQUE;
}

bool SmartPointerAnalyzer::classNeedsSharedFromThis(const std::string& className) const {
    return classesNeedingSharedFromThis_.find(className) != classesNeedingSharedFromThis_.end();
}

void SmartPointerAnalyzer::scanContainerElementKinds(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            for (const auto& inner : fn->body) scanContainerElementKindsInStatement(*inner, fn->name);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            for (const auto& inner : mn->body) scanContainerElementKindsInStatement(*inner, "main");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    const std::string scope = cls->name + "::" + method->name;
                    for (const auto& inner : method->body) scanContainerElementKindsInStatement(*inner, scope);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    const std::string scope = cls->name + "::" + ctor->name;
                    for (const auto& inner : ctor->body) scanContainerElementKindsInStatement(*inner, scope);
                }
            }
        }
    }
}

namespace {
// Local helper: does `expr` name a bare identifier, and if so what is it? Used to
// resolve both the push_back receiver (container variable) and argument (source
// variable) without needing full expression evaluation -- this scan only needs the
// direct, common shape `container.push_back(sourceVar)`.
const Identifier* asIdentifier(const Expression* expr) {
    return dynamic_cast<const Identifier*>(expr);
}
} // namespace

void SmartPointerAnalyzer::scanContainerElementKindsInStatement(const Statement& stmt,
                                                                  const std::string& scopeName) {
    // Only need to find `<container>.push_back(<arg>)` call expressions, wherever
    // they appear (as a bare expression statement or nested in an initializer) --
    // recurse into every statement shape that can contain one.
    auto visitExpr = [this, &scopeName](const Expression* expr) {
        auto call = dynamic_cast<const FunctionCall*>(expr);
        if (call == nullptr || !call->callee) return;
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr || member->member != "push_back" || call->arguments.empty()) return;
        const Identifier* recv = asIdentifier(member->object.get());
        const Identifier* arg = asIdentifier(call->arguments[0].get());
        if (recv == nullptr || arg == nullptr) return;

        // Resolve the argument's OWN already-decided pointer kind via its symbol
        // type (registered the same way locals/parameters/fields already are).
        // Precise scope first -- see scanContainerElementKindsInStatement's
        // header comment for why the kGlobalScope fallback alone collides
        // across unrelated functions/methods sharing a local's bare name.
        auto typeIt = symbolTypesByScope.find(makeSymbolKey(scopeName, arg->name));
        if (typeIt == symbolTypesByScope.end()) {
            typeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, arg->name));
        }
        if (typeIt == symbolTypesByScope.end()) return;
        if (getPointerType(typeIt->second, arg->name) == PointerType::SHARED) {
            containerElementShared_[recv->name] = true;
        } else if (containerElementShared_.find(recv->name) == containerElementShared_.end()) {
            containerElementShared_[recv->name] = false;
        }
    };

    if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(&stmt)) {
        visitExpr(exprStmt->expression.get());
    } else if (auto decl = dynamic_cast<const VariableDeclaration*>(&stmt)) {
        if (decl->expression) visitExpr(decl->expression.get());
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(&stmt)) {
        if (assign->expression) visitExpr(assign->expression.get());
    } else if (auto ifs = dynamic_cast<const IfStatement*>(&stmt)) {
        for (const auto& inner : ifs->body) scanContainerElementKindsInStatement(*inner, scopeName);
        for (const auto& inner : ifs->elseBody) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(&stmt)) {
        for (const auto& inner : whiles->body) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto fors = dynamic_cast<const ForStatement*>(&stmt)) {
        for (const auto& inner : fors->body) scanContainerElementKindsInStatement(*inner, scopeName);
    }
}

namespace {
// Every `return <expr>;` reachable from `body`, recursing through if/while/for
// the same way bodyReturnsThis does. Collected once per method and re-tested
// against the current partial classification on each fixed-point round below.
void collectReturnExpressions(const std::vector<StatementPtr>& body,
                              std::vector<const Expression*>& out) {
    for (const auto& stmtPtr : body) {
        if (auto ret = dynamic_cast<const ReturnStatement*>(stmtPtr.get())) {
            out.push_back(ret->expression.get());
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmtPtr.get())) {
            collectReturnExpressions(ifs->body, out);
            collectReturnExpressions(ifs->elseBody, out);
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmtPtr.get())) {
            collectReturnExpressions(whiles->body, out);
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmtPtr.get())) {
            collectReturnExpressions(fors->body, out);
        }
    }
}

// True if `this` (grammar.js's reserved `this_expression`, ast.h's
// ThisExpression) appears ANYWHERE in `body` as a call argument, a
// VariableDeclaration RHS, or an AssignmentStatement RHS -- the three
// positions AstEmitter::emitMovedArg lowers to shared_from_this() (see
// Chapter 18 Step 37; `return this;` is deliberately NOT one of these, since
// it is already covered by the separate anyReturnsThis scan above, which
// only looks at RETURN expressions).
//
// A call argument is checked at ANY nesting depth (`wrap(this)` inside a
// bigger expression, inside an if/while/for/try body) -- not just when the
// call is the top-level statement -- because emitMovedArg fires wherever
// AstEmitter's own expression emission encounters a FunctionCall, at
// whatever depth. Each argument is checked as being DIRECTLY `this` (not
// merely containing it somewhere further inside), matching emitMovedArg's
// own exact-shape check.
bool methodPassesThisAsArgument(const std::vector<StatementPtr>& body) {
    std::function<bool(const Expression*)> exprHasThisArg = [&](const Expression* expr) -> bool {
        if (expr == nullptr) return false;
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            for (const auto& arg : call->arguments) {
                if (dynamic_cast<const ThisExpression*>(arg.get()) != nullptr) return true;
            }
            for (const auto& arg : call->arguments) {
                if (exprHasThisArg(arg.get())) return true;
            }
            return call->callee && exprHasThisArg(call->callee.get());
        }
        if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
            for (const auto& arg : neww->arguments) {
                if (dynamic_cast<const ThisExpression*>(arg.get()) != nullptr) return true;
                if (exprHasThisArg(arg.get())) return true;
            }
            return false;
        }
        if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            return exprHasThisArg(bin->left.get()) || exprHasThisArg(bin->right.get());
        }
        if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            return exprHasThisArg(un->operand.get());
        }
        if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
            return exprHasThisArg(member->object.get());
        }
        if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            return exprHasThisArg(sub->object.get()) || exprHasThisArg(sub->index.get());
        }
        return false;
    };

    std::function<bool(const std::vector<StatementPtr>&)> walkBody =
        [&](const std::vector<StatementPtr>& stmts) -> bool {
        for (const auto& stmtPtr : stmts) {
            const Statement* stmt = stmtPtr.get();
            if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (dynamic_cast<const ThisExpression*>(decl->expression.get()) != nullptr) return true;
                if (exprHasThisArg(decl->expression.get())) return true;
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (dynamic_cast<const ThisExpression*>(assign->expression.get()) != nullptr) return true;
                if (exprHasThisArg(assign->expression.get())) return true;
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (exprHasThisArg(exprStmt->expression.get())) return true;
            } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                // NOT checking whether the return's own top-level expression
                // IS `this` directly (`return this;`) -- that shape is the
                // separate anyReturnsThis scan's job, above. This only needs
                // to catch `this` NESTED inside a larger returned expression
                // (`return r.readVal(this);`), which anyReturnsThis's own
                // narrower `dynamic_cast<ThisExpression>` check on the whole
                // return expression would miss entirely.
                if (exprHasThisArg(ret->expression.get())) return true;
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (exprHasThisArg(ifs->condition.get())) return true;
                if (walkBody(ifs->body) || walkBody(ifs->elseBody)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (exprHasThisArg(whiles->condition.get())) return true;
                if (walkBody(whiles->body)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                if (exprHasThisArg(fors->container.get())) return true;
                if (walkBody(fors->body)) return true;
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (walkBody(trys->body) || walkBody(trys->catchBody)) return true;
            }
        }
        return false;
    };

    return walkBody(body);
}
} // namespace

namespace {
// Resolves an expression reached inside a `new SameClass(<expr>)` return
// (case (d) of inferAliasReturningMethods, below) down to the (declaredType,
// fieldName) pair it ultimately reads, crossing class boundaries via
// classMembersByClass_ as needed (unlike cases (a)-(c), which stay within a
// single class's own fieldTypes map). Handles exactly the shapes this
// pattern's own recursive walkers produce: a bare field/`this->field` (the
// base case), or `<chain>.<method>(...)` where <method> is already known
// alias-returning on <chain>'s resolved class (methodReturnKinds_, populated
// for every class processed earlier in program.statements' source order --
// safe because a handle class wrapping a payload class is always declared
// AFTER that payload class, the same "no forward references" constraint
// this codebase's whole design already works within). Returns nullopt for
// any shape it cannot resolve (conservative: case (d) then fails closed,
// same as every other case here).
std::optional<std::pair<std::string, std::string>> resolveInnerFieldChain(
    const Expression* expr,
    const std::string& enclosingClass,
    const std::map<std::string, std::map<std::string, std::string>>& classMembersByClass,
    const std::map<std::string, PointerType>& methodReturnKinds) {
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        auto clsIt = classMembersByClass.find(enclosingClass);
        if (clsIt == classMembersByClass.end()) return std::nullopt;
        auto fIt = clsIt->second.find(id->name);
        if (fIt == clsIt->second.end()) return std::nullopt;
        return std::make_pair(fIt->second, id->name);
    }
    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
        // `this->field` / `recv.field`: resolve the receiver's class first
        // (bare identifier receiver only -- matches this whole pass's
        // existing conservatism elsewhere), then look up `member->member`
        // in THAT class's field map.
        std::string receiverClass;
        if (dynamic_cast<const ThisExpression*>(member->object.get()) != nullptr) {
            receiverClass = enclosingClass;
        } else if (auto recv = dynamic_cast<const Identifier*>(member->object.get())) {
            auto clsIt = classMembersByClass.find(enclosingClass);
            if (clsIt != classMembersByClass.end()) {
                if (auto fIt = clsIt->second.find(recv->name); fIt != clsIt->second.end()) {
                    receiverClass = fIt->second;
                }
            }
        } else if (auto innerMember = dynamic_cast<const MemberAccess*>(member->object.get())) {
            // `a.b.c` -- resolve `a.b`'s class recursively first.
            const auto inner = resolveInnerFieldChain(innerMember, enclosingClass, classMembersByClass,
                                                        methodReturnKinds);
            if (!inner.has_value()) return std::nullopt;
            receiverClass = inner->first;
        }
        if (receiverClass.empty()) return std::nullopt;
        auto clsIt = classMembersByClass.find(receiverClass);
        if (clsIt == classMembersByClass.end()) return std::nullopt;
        auto fIt = clsIt->second.find(member->member);
        if (fIt == clsIt->second.end()) return std::nullopt;
        return std::make_pair(fIt->second, member->member);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        auto callMember = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (callMember == nullptr) return std::nullopt;
        // Resolve the receiver chain's class, then require the called method
        // to already be classified alias-returning ON THAT CLASS.
        std::string receiverClass;
        if (dynamic_cast<const ThisExpression*>(callMember->object.get()) != nullptr) {
            receiverClass = enclosingClass;
        } else if (auto recv = dynamic_cast<const Identifier*>(callMember->object.get())) {
            auto clsIt = classMembersByClass.find(enclosingClass);
            if (clsIt != classMembersByClass.end()) {
                if (auto fIt = clsIt->second.find(recv->name); fIt != clsIt->second.end()) {
                    receiverClass = fIt->second;
                }
            }
        } else {
            const auto inner = resolveInnerFieldChain(callMember->object.get(), enclosingClass,
                                                        classMembersByClass, methodReturnKinds);
            if (!inner.has_value()) return std::nullopt;
            receiverClass = inner->first;
        }
        if (receiverClass.empty()) return std::nullopt;
        const std::string key = makeMemberKey(receiverClass, callMember->member);
        auto kindIt = methodReturnKinds.find(key);
        if (kindIt == methodReturnKinds.end() || kindIt->second != PointerType::SHARED) return std::nullopt;
        // The call resolves to an alias of the RECEIVER's own class chain --
        // the field ultimately being aliased is the receiver's OWN resolved
        // field, so recurse into the receiver itself (same class, no field
        // name to add beyond what the receiver already names).
        return resolveInnerFieldChain(callMember->object.get(), enclosingClass, classMembersByClass,
                                       methodReturnKinds);
    }
    return std::nullopt;
}
} // namespace

void SmartPointerAnalyzer::inferAliasReturningMethods(const Program& program) {
    // A method M is ALIAS_RETURNING (return type shared_ptr<T>, inferred, no
    // annotation -- CLAUDE.md's "ownership is inferred, not annotated" applies
    // to method returns exactly as it does to fields) iff EVERY return in its
    // body is one of:
    //   (a) `return this;`
    //   (b) `return <field>;` -- a bare identifier or `this->field` naming an
    //       object-typed field of the SAME class (never `new T(...)`, never
    //       any other expression shape)
    //   (c) `return <recv>.<method>(...);` where <method> is ANOTHER method
    //       already classified ALIAS_RETURNING (the recursive-walker call
    //       shape yaml-cpp's Node::findOrAppendSibling/siblingAt use)
    // This is a genuine small fixed-point: `operator[]` calls
    // findOrAppendSibling/siblingAt (case c), so those two "leaf" methods
    // (which only ever hit cases a/b) must be classified FIRST. Bounded by
    // method count per class -- classification only ever flips
    // undecided -> decided, never back, so this always terminates.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;

        // className.fieldName -> is this field object-typed (case (b) is only
        // legal for an object-typed field; a primitive field is never an alias).
        std::map<std::string, std::string> fieldTypes;
        for (const auto& member : cls->members) {
            if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
                fieldTypes[var->identifier] = var->type;
            }
        }

        std::vector<const MethodDeclaration*> methods;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                // A specialization (in-class OR spliced in externally, see
                // ASTBuilder's isExternalSpecialization) is excluded: every
                // set/map in this whole function (aliasReturning,
                // notAliasReturning, and the methodReturnKinds_ write at the
                // bottom) is keyed by BARE method NAME, with no notion of
                // specializationKey -- a specialization sharing a name with
                // its primary (or another specialization) would silently
                // corrupt the PRIMARY's own classification (confirmed as a
                // real bug via probe: `Point Node::as<Point>() { Point out
                // = ...; return out; }`, an external specialization, flipped
                // `Node::as`'s registered kind, which in turn corrupted an
                // unrelated call chain -- YAML::Load -> Parser::
                // parseDocument -- reading that same bare key). A
                // specialization's return lowering is a wholly separate,
                // already-correct concern (AstEmitter::
                // currentSpecializationValueType_), so this pass has
                // nothing to contribute for one.
                if (!method->specializationArg.empty()) continue;
                methods.push_back(method);
            }
        }

        std::set<std::string> aliasReturning;     // method names decided ALIAS_RETURNING this round
        std::set<std::string> notAliasReturning;  // method names decided NOT this round
        std::set<std::string> fieldsNeedingShared;
        // Case (d)'s fields, resolved with their OWN (possibly different-class)
        // declared type already attached -- see resolveInnerFieldChain. Kept
        // separate from fieldsNeedingShared (whose entries are bare names
        // resolved against THIS class's fieldTypes map at the write site
        // below) since case (d)'s fields may belong to an entirely different
        // class (NodeData, not Node).
        std::vector<std::pair<std::string, std::string>> fieldsNeedingSharedTyped;

        bool progressed = true;
        while (progressed) {
            progressed = false;
            for (const auto* method : methods) {
                if (aliasReturning.count(method->name) || notAliasReturning.count(method->name)) {
                    continue;  // already classified
                }
                std::vector<const Expression*> returns;
                collectReturnExpressions(method->body, returns);
                if (returns.empty()) {
                    notAliasReturning.insert(method->name);
                    progressed = true;
                    continue;
                }

                bool allResolved = true;   // every return matches a known case
                bool anyUndecided = false; // at least one return depends on a
                                           // not-yet-classified method (retry later)
                std::set<std::string> fieldsThisMethod;
                for (const Expression* expr : returns) {
                    if (dynamic_cast<const ThisExpression*>(expr) != nullptr) continue;  // case (a)
                    if (auto id = dynamic_cast<const Identifier*>(expr)) {
                        auto fIt = fieldTypes.find(id->name);
                        if (fIt != fieldTypes.end() && classNames.count(fIt->second) > 0) {
                            fieldsThisMethod.insert(id->name);  // case (b), bare
                            continue;
                        }
                        allResolved = false;
                        break;
                    }
                    if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
                        // `this->field` / `this.field` (a bare-field-access, not
                        // itself the whole return value -- that's case (a) above).
                        if (dynamic_cast<const ThisExpression*>(member->object.get()) != nullptr) {
                            auto fIt = fieldTypes.find(member->member);
                            if (fIt != fieldTypes.end() && classNames.count(fIt->second) > 0) {
                                fieldsThisMethod.insert(member->member);  // case (b)
                                continue;
                            }
                        }
                        allResolved = false;
                        break;
                    }
                    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
                        // case (c): `<recv>.<method>(...)` (explicit receiver,
                        // `call->callee` a MemberAccess) OR a bare `<method>(...)`
                        // with an implicit `this` receiver (`call->callee` null,
                        // name in call->function_name) -- either shape counts,
                        // since the rule is about the CALLED method's own
                        // classification, not which variable/field is calling it.
                        std::string calledName;
                        if (auto callMember = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                            calledName = callMember->member;
                        } else if (!call->callee) {
                            calledName = call->function_name;
                        }
                        // Direct or implicit-this self-recursion (`return
                        // next.findOrAppend(k);` inside findOrAppend itself, or
                        // `return findOrAppend(k);`) can never block its own
                        // classification: if every OTHER return in this method
                        // matches the alias-returning shape, the recursive call
                        // is consistent with (never refutes) that conclusion.
                        // Without this, a method that only ever calls itself
                        // (the exact walker shape this rule exists for) would
                        // wait forever on its own not-yet-decided classification
                        // and default to NOT alias-returning -- confirmed as a
                        // real bug via a direct probe before this fix.
                        if (!calledName.empty() && calledName == method->name) {
                            continue;
                        }
                        if (!calledName.empty() && aliasReturning.count(calledName) > 0) {
                            continue;
                        }
                        if (!calledName.empty() && notAliasReturning.count(calledName) > 0) {
                            allResolved = false;
                            break;
                        }
                        // Callee not yet classified -- retry this method on a
                        // later round if it names a same-class method; otherwise
                        // (calls something outside this class) this return can
                        // never resolve to alias.
                        if (!calledName.empty()) {
                            bool calleeIsSameClassMethod = false;
                            for (const auto* m : methods) {
                                if (m->name == calledName) { calleeIsSameClassMethod = true; break; }
                            }
                            if (calleeIsSameClassMethod) { anyUndecided = true; continue; }
                        }
                        allResolved = false;
                        break;
                    }
                    // case (d): `return new T(<inner>);` where T is the method's
                    // OWN class (a HANDLE constructor wrapping an existing payload,
                    // e.g. `Node::operator[]`'s `return new Node(data.child...);`
                    // -- the RB18 handle/payload split's shape: a method on the
                    // HANDLE class (Node) returns a FRESH handle wrapping a field
                    // reached through a DIFFERENT class's field chain (NodeData's
                    // `child`/`next`, via Node's own `data` field), not a field of
                    // its own class the way cases (a)-(c) assume. `<inner>` is
                    // resolved the SAME way a return value is (bare identifier /
                    // `x.field` chain / a call to an already-alias-returning
                    // method), but walks classMembersByClass_ across WHATEVER
                    // class each step's field actually belongs to (not just this
                    // method's own class), since the chain crosses the Node/
                    // NodeData boundary. Whatever field the chain terminates in is
                    // recorded in fieldsNeedingShared keyed by ITS OWN declared
                    // type (resolveInnerFieldChain returns (fieldType, fieldName)
                    // directly, unlike fieldsThisMethod's bare names which rely on
                    // the current method's fieldTypes map) -- this is exactly what
                    // makeMemberKey needs, and mirrors the existing invariant that
                    // this map is keyed by "field's own type", not "containing
                    // class" (see the real bug this same invariant fixed, in the
                    // comment below at the explicitHints write site).
                    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
                        if (neww->className == cls->name && neww->arguments.size() == 1) {
                            const auto resolved = resolveInnerFieldChain(
                                neww->arguments[0].get(), cls->name, classMembersByClass_, methodReturnKinds_);
                            if (resolved.has_value()) {
                                fieldsNeedingSharedTyped.push_back(*resolved);
                                continue;
                            }
                        }
                        allResolved = false;
                        break;
                    }
                    // any other expression shape -> not alias-returning.
                    allResolved = false;
                    break;
                }

                if (!allResolved) {
                    notAliasReturning.insert(method->name);
                    progressed = true;
                } else if (!anyUndecided) {
                    aliasReturning.insert(method->name);
                    fieldsNeedingShared.insert(fieldsThisMethod.begin(), fieldsThisMethod.end());
                    progressed = true;
                }
                // else: still depends on an undecided sibling method -- leave
                // unclassified, retry next round.
            }
        }
        // Anything still unclassified after the loop stabilizes (a genuine
        // mutual-recursion cycle between two never-resolving methods) defaults
        // to NOT alias-returning -- the same conservative "ambiguous -> safe
        // default" CeeLing applies elsewhere, not a silent guess toward SHARED.

        // Dual-register under the QUALIFIED name too (`YAML::Node` alongside
        // `Node`), matching every other collection pass in this codebase --
        // `exprObjectClass`/`resolveDeclaredType`-equivalent lookups in
        // AstEmitter/DanglingChecker key by the RECEIVER's declared type
        // string, which is "YAML::Node" for a `YAML::Node n;` local. Without
        // this, `n["key"]`'s shared-return classification would silently miss
        // for any namespaced class (confirmed as a real bug via the actual
        // yaml_node.clg: `root["name"]` kept emitting unique_ptr because
        // "YAML::Node" was never a key in methodReturnKinds_/explicitHints,
        // only the bare "Node" was).
        std::vector<std::string> classKeys = {cls->name};
        if (!cls->namespaceName.empty()) {
            classKeys.push_back(cls->namespaceName + "::" + cls->name);
        }
        for (const auto& name : aliasReturning) {
            for (const auto& classKey : classKeys) {
                methodReturnKinds_[makeMemberKey(classKey, name)] = PointerType::SHARED;
            }
        }
        // A field returned by an alias-returning method needs the SAME SHARED
        // decision every other field lookup already goes through
        // (getPointerType consults this exact map) -- reuses explicitHints
        // itself rather than adding a parallel map. CRITICAL: explicitHints'
        // key convention is "<declared TYPE of the thing>.<variable name>"
        // (confirmed via `//@ptr shared Node.a` in
        // vector_push_back_shares_when_already_shared_accepted.clg, where `a`
        // is a LOCAL of type Node -- "Node" there is a's TYPE, not some
        // enclosing scope), matching VariableReference::className's own
        // semantics (the field/local's TYPE, not its containing class) that
        // secondPassDeterminePointerTypes's lookup (`makeMemberKey(ref.
        // className, ref.variableName)`) actually queries. Keying by the
        // CONTAINING class (classKey, same as methodReturnKinds_ correctly
        // does -- but methodReturnKinds_'s query is genuinely
        // (containingClass, methodName), a different lookup with different
        // semantics) was a real bug: `Holder.cur` never matched anything,
        // because getPointerType's real query for a `Box cur;` field is
        // keyed "Box.cur", not "Holder.cur" -- confirmed via a direct probe
        // (a field returned by an alias-returning method kept emitting
        // unique_ptr while the method itself correctly emitted shared_ptr,
        // a real return-type mismatch compile error). Fixed by keying on the
        // field's own declared TYPE (fieldTypes[fieldName]) instead.
        // Deliberately does NOT touch explicitHintLines (no source line
        // exists for an inferred decision): nothing reads explicitHintLines
        // except applyExplicitHints's own "did every USER-WRITTEN hint match
        // something" diagnostic sweep, which iterates explicitHintLines, not
        // explicitHints -- so these inferred entries never trigger a
        // spurious "hint didn't match" diagnostic.
        for (const auto& fieldName : fieldsNeedingShared) {
            explicitHints[makeMemberKey(fieldTypes[fieldName], fieldName)] = PointerType::SHARED;
        }
        // Case (d)'s fields already carry their own (declaredType, fieldName)
        // pair (possibly a DIFFERENT class than cls->name -- see
        // resolveInnerFieldChain), so write directly rather than looking the
        // type up in this class's own fieldTypes map.
        for (const auto& [fieldType, fieldName] : fieldsNeedingSharedTyped) {
            explicitHints[makeMemberKey(fieldType, fieldName)] = PointerType::SHARED;
        }
        for (const auto& name : aliasReturning) {
            const bool anyReturnsThis = [&] {
                for (const auto* method : methods) {
                    if (method->name != name) continue;
                    std::vector<const Expression*> returns;
                    collectReturnExpressions(method->body, returns);
                    for (const Expression* expr : returns) {
                        if (dynamic_cast<const ThisExpression*>(expr) != nullptr) {
                            return true;
                        }
                    }
                }
                return false;
            }();
            // A VALUE-TYPE (struct) class's `return this;` already gets a
            // completely different, correct lowering (`return *this;` --
            // AstEmitter's currentMethodReturnsValueType_ branch) that needs
            // no shared_ptr machinery at all. Found via a real probe:
            // imgui-cling's `Response` (a plain value struct, never
            // smart-pointer-wrapped anywhere) gained a chainable
            // `onHoverText()` method (`return this;`, matching real egui's
            // own `Response::on_hover_text(self, ...) -> Self` fluent-chain
            // shape) and wrongly picked up `class Response : public
            // std::enable_shared_from_this<Response>` as a spurious base --
            // harmless in that it happened to still compile (unused base,
            // no crash), but semantically wrong for a type this project's
            // own design explicitly never wraps in a smart pointer.
            if (anyReturnsThis && !isValueTypeClass(cls->name)) {
                // A class returning `this` as a shared_ptr must itself ALWAYS be
                // constructed as shared_ptr (shared_from_this() requires the
                // object to already be owned by a shared_ptr) -- reuses the exact
                // mechanism classesNeedingSharedRoots already provides for
                // back-reference/bidirectional cases, so ordinary `new ClassName()`
                // sites need no changes to pick up make_shared.
                classesNeedingSharedFromThis_.insert(cls->name);
                classesNeedingSharedRoots.insert(cls->name);
            }
        }
        // A SEPARATE scan, over EVERY method (not just ones already classified
        // `aliasReturning` above -- passing `this` as an argument has nothing to
        // do with a method's OWN return value, so it must not be gated on that
        // classification): does any method pass `this` as a call argument, or
        // store it via a bare `Node y = this;`/`y = this;`? Chapter 18 Step 37 --
        // AstEmitter::emitMovedArg lowers any of those three shapes to
        // shared_from_this(), which needs the SAME enable_shared_from_this +
        // forced-shared-root machinery `return this;` above already triggers.
        // Skipping this scan would leave a `this`-passing class with NO base
        // class inheriting enable_shared_from_this at all (a "not a member"
        // compile error) or, worse, an object not actually shared_ptr-owned
        // (shared_from_this()'s own std::bad_weak_ptr throw at runtime).
        // Same value-type exclusion as the return-position scan above --
        // a value-type class's `this` never needs shared_ptr machinery.
        if (!isValueTypeClass(cls->name)) {
            for (const auto* method : methods) {
                if (methodPassesThisAsArgument(method->body)) {
                    classesNeedingSharedFromThis_.insert(cls->name);
                    classesNeedingSharedRoots.insert(cls->name);
                }
            }
        }
    }
}

void SmartPointerAnalyzer::inferAliasReturningFreeFunctions(const Program& program) {
    // Free-function counterpart of inferAliasReturningMethods's case (c): a
    // free function whose body ONLY ever returns a call to an ALREADY-
    // classified alias-returning method/function is itself alias-returning.
    // Needed for yaml-cling's `Node Load(string text) { Parser p = new
    // Parser(text); return p->parseDocument(); }`.
    //
    // MUST run AFTER promoteReturnsOfSharedLocals, not alongside
    // inferAliasReturningMethods (where an earlier version of this lived) --
    // parseDocument (a CLASS method) only becomes SHARED via
    // promoteReturnsOfSharedLocals (it returns a plain local, `root`, that
    // was itself promoted to SHARED by the even-later promoteSharedCallArguments
    // pass), which runs AFTER inferAliasReturningMethods entirely. Running
    // this free-function check too early meant Load's own call to
    // parseDocument() queried a classification that did not exist yet, and
    // Load silently stayed unique_ptr while parseDocument was correctly
    // shared_ptr -- a real return-type mismatch confirmed via probe before
    // this pass was pulled out into its own, later-running function.
    std::set<std::string> aliasReturningFreeFunctions;
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        if (classNames.find(fn->returnType) == classNames.end()) continue;

        std::vector<const Expression*> returns;
        collectReturnExpressions(fn->body, returns);
        if (returns.empty()) continue;

        bool allAliasCalls = true;
        for (const Expression* expr : returns) {
            auto call = dynamic_cast<const FunctionCall*>(expr);
            if (call == nullptr) { allAliasCalls = false; break; }
            std::string calledClass;
            std::string calledName;
            if (call->callee) {
                auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
                if (member == nullptr) { allAliasCalls = false; break; }
                if (auto recvId = dynamic_cast<const Identifier*>(member->object.get())) {
                    // Try THIS free function's own scope first (registerSymbolType
                    // registers every local under its real, precise scope --
                    // analyzeStatement already passes fn->name as that scope for
                    // a free function's body -- in ADDITION to a same-name
                    // kGlobalScope fallback entry). Two free functions each
                    // declaring a local of the same NAME but different TYPES
                    // (e.g. Load's `Parser p` vs main's `Point p`) collide in
                    // the kGlobalScope fallback, since it is last-write-wins
                    // with no scoping at all -- confirmed as a real bug via
                    // probe (main's `Point p` silently corrupted Load's OWN
                    // resolution of its own unrelated `p.parseDocument()`
                    // call). The precise scope has no such collision.
                    auto typeIt = symbolTypesByScope.find(makeSymbolKey(fn->name, recvId->name));
                    if (typeIt == symbolTypesByScope.end()) {
                        typeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, recvId->name));
                    }
                    if (typeIt != symbolTypesByScope.end()) calledClass = typeIt->second;
                }
                calledName = member->member;
            } else {
                calledName = call->function_name;
            }
            if (calledClass.empty()) {
                if (aliasReturningFreeFunctions.count(calledName) == 0) { allAliasCalls = false; break; }
            } else if (getMethodReturnPointerType(calledClass, calledName) != PointerType::SHARED) {
                allAliasCalls = false;
                break;
            }
        }
        if (allAliasCalls) {
            aliasReturningFreeFunctions.insert(fn->name);
            methodReturnKinds_[makeMemberKey("", fn->name)] = PointerType::SHARED;
            // Dual-register under the QUALIFIED name too (`YAML::Load`) --
            // isSharedReturningCall's plain-call check (deciding a LOCAL
            // declaration's pointer kind, e.g. `Node root = YAML::Load(x);`)
            // queries call->function_name verbatim, which IS the qualified
            // spelling at a call site outside the namespace, unlike
            // emitFunction's DEFINITION-side query (always the bare name).
            // Confirmed as a real bug via probe: Load itself correctly
            // emitted shared_ptr, but `Node root = YAML::Load(...);` still
            // declared root as unique_ptr because only the bare "Load" key
            // existed.
            if (!fn->namespaceName.empty()) {
                methodReturnKinds_[makeMemberKey("", fn->namespaceName + "::" + fn->name)] =
                    PointerType::SHARED;
            }
        }
    }
}

void SmartPointerAnalyzer::promoteReturnsOfSharedLocals(const Program& program) {
    // A method's return classification (inferAliasReturningMethods, above) is
    // structural: it only recognizes `return this;` / `return <field>;` /
    // `return <alias-returning call>;`. It does NOT recognize `return <local
    // variable>;` -- and a local can independently become SHARED via a LATER
    // pass (promoteSharedCallArguments, the very last pass in analyzeProgram,
    // since a local passed into a parameter that itself got promoted to
    // SHARED must become SHARED too). Found as a real gap while writing
    // yaml-cling's Parser: `Node parseDocument() { Node root = new Node(...);
    // parseValueInto(root, -1); return root; }` -- `root` gets promoted to
    // shared_ptr (it's passed into parseValueInto's `target` parameter, which
    // itself needs SHARED because the body does `target[key]`, an alias-
    // returning operator[] call), but parseDocument's OWN return type stayed
    // unique_ptr, a real return-type mismatch compile error. This pass
    // MUST run after promoteSharedCallArguments (needs every local's FINAL
    // pointer-kind decision, not the one from the ordinary two-pass alone),
    // and reuses getPointerTypeForVariable rather than re-deriving locals'
    // kinds from scratch.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr) continue;
            // A specialization (in-class `int as<int>()` OR an external
            // one spliced in via ASTBuilder, e.g. `Point Node::as<Point>()`)
            // is skipped here -- this pass keys methodReturnKinds_ by BARE
            // method name (makeMemberKey(cls->name, method->name)), with no
            // notion of specializationKey at all, so a specialization whose
            // return type happens to be a real class (`Point`, unlike the
            // primary's literal `T`) would silently corrupt the PRIMARY's
            // own "Node::as" classification -- confirmed as a real bug via
            // probe: adding `Point Node::as<Point>() { Point out = ...;
            // return out; }` flipped `Node::as`'s registered kind to SHARED
            // (because `out`, a specialization-local never registered the
            // way this pass's variable-scope lookup expects, fell through
            // getPointerTypeForVariableInScope's own "not found -> SHARED"
            // default quirk), which in turn corrupted an UNRELATED call
            // chain (YAML::Load -> Parser::parseDocument) that happened to
            // read the same "Node::as" key. A specialization's own return
            // lowering is a completely separate, already-correct concern
            // (AstEmitter::currentSpecializationValueType_), so this pass
            // has nothing useful to contribute for one and must not run on
            // it at all.
            if (!method->specializationArg.empty()) continue;
            // Only meaningful for a CLASS-typed return -- getPointerTypeForVariable
            // defaults to SHARED for any name it can't match at all (a real,
            // separate quirk of that query, documented at its own definition),
            // so restricting to class-typed returns keeps this pass from ever
            // acting on a coincidentally-named primitive local.
            if (classNames.find(method->returnType) == classNames.end()) continue;
            const std::string key = makeMemberKey(cls->name, method->name);
            if (methodReturnKinds_.count(key) > 0) continue;  // already SHARED

            std::vector<const Expression*> returns;
            collectReturnExpressions(method->body, returns);
            bool anyLocalIsShared = false;
            // Scoped lookup ("Class::method") -- getPointerTypeForVariable's
            // plain name-only match would wrongly collide with a same-named
            // local in a COMPLETELY UNRELATED method/function elsewhere in the
            // program (confirmed via a real bug: two free functions each with
            // their own local named `copy` cross-contaminated each other's
            // classification -- see getPointerTypeForVariableInScope's own
            // comment).
            const std::string methodScope = cls->name + "::" + method->name;
            for (const Expression* expr : returns) {
                // `return this;` is never a SHARED-local return in this sense (it's
                // handled by the separate anyReturnsThis/classesNeedingSharedFromThis_
                // mechanism above) -- `this` is its own ThisExpression node now, so
                // dynamic_cast<Identifier> already returns nullptr for it, same as
                // any other non-Identifier return shape.
                auto id = dynamic_cast<const Identifier*>(expr);
                if (id == nullptr) continue;
                if (getPointerTypeForVariableInScope(methodScope, id->name) == PointerType::SHARED) {
                    anyLocalIsShared = true;
                    break;
                }
            }
            if (anyLocalIsShared) {
                methodReturnKinds_[key] = PointerType::SHARED;
                if (!cls->namespaceName.empty()) {
                    methodReturnKinds_[makeMemberKey(cls->namespaceName + "::" + cls->name, method->name)] =
                        PointerType::SHARED;
                }
            }
        }
    }

    // Same check for top-level FREE FUNCTIONS, keyed under the "" class
    // (matching how getMethodReturnPointerType/emitFunction now query a free
    // function -- see AstEmitter::emitFunction's ownerClass="" case). Needed
    // for `Node Load(string text) { ... return p->parseDocument(); }`-shaped
    // code: this pass alone doesn't classify Load as alias-returning (Load
    // returns a CALL RESULT, not a bare local -- that's
    // inferAliasReturningMethods's case (c), not this pass's concern), but
    // any free function that DOES return a shared local needs the same
    // promotion class methods just got above.
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        if (classNames.find(fn->returnType) == classNames.end()) continue;
        const std::string key = makeMemberKey("", fn->name);
        if (methodReturnKinds_.count(key) > 0) continue;

        std::vector<const Expression*> returns;
        collectReturnExpressions(fn->body, returns);
        bool anyLocalIsShared = false;
        // Scoped lookup (bare fn->name, matching registerParameters'
        // scopeName for a free function) -- see the class-method loop above
        // for why this must not be the plain name-only getPointerTypeForVariable
        // (a real cross-function name collision, confirmed via yaml-cling's
        // Clone/cloneChain both having their own local named `copy`).
        for (const Expression* expr : returns) {
            auto id = dynamic_cast<const Identifier*>(expr);
            if (id == nullptr) continue;
            if (getPointerTypeForVariableInScope(fn->name, id->name) == PointerType::SHARED) {
                anyLocalIsShared = true;
                break;
            }
        }
        if (anyLocalIsShared) {
            // Keyed under the BARE name ("" class + fn->name), matching
            // exactly how AstEmitter::buildFunctionSignature queries it on the
            // DEFINITION side -- emitTopLevel always passes fn->name verbatim,
            // never a namespace-qualified spelling, for ownerClass="" free
            // functions (unlike class methods, which route through
            // emitClass's namespace re-wrapping instead of the name itself
            // carrying the qualifier).
            //
            // ALSO dual-registered under the QUALIFIED name -- a real, confirmed
            // gap found via yaml-cling's `YAML::Clone`: the CALLER-side query
            // (AstEmitter::isSharedReturningCall, deciding a fresh local's OWN
            // declared type from a call's RHS, e.g. `Node copy =
            // YAML::Clone(root);`) uses call->function_name VERBATIM, which is
            // the qualified spelling from outside the namespace -- exactly the
            // same bug class inferAliasReturningFreeFunctions's own dual-
            // registration comment already documents for YAML::Load, this
            // pass just hadn't needed it until a function returning a locally-
            // shared variable (rather than a call result) was namespaced too.
            methodReturnKinds_[key] = PointerType::SHARED;
            if (!fn->namespaceName.empty()) {
                methodReturnKinds_[makeMemberKey("", fn->namespaceName + "::" + fn->name)] =
                    PointerType::SHARED;
            }
        }
    }
}

void SmartPointerAnalyzer::promoteParamsStoredInSharedFields(const Program& program) {
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                promoteParamsInBody(method->body, cls->name, method->parameters);
            } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                promoteParamsInBody(ctor->body, cls->name, ctor->parameters);
            }
        }
    }
}

void SmartPointerAnalyzer::promoteParamsInBody(const std::vector<StatementPtr>& body,
                                               const std::string& className,
                                               const std::vector<std::pair<std::string, std::string>>& params) {
    // Only the unconditional, direct, single-hop case is promoted this slice: a
    // top-level `field = param;` (bare identifier target naming a field of
    // `className`, or a `this`-qualified member-access target -- CeeLing has no
    // explicit `this.x` syntax, so only the bare form is reachable in practice)
    // whose RHS is EXACTLY a parameter name. Storage behind a conditional, into a
    // nested field, or via an intermediate expression is NOT detected -- the
    // argument stays UNIQUE and still moves, the existing safe default (see
    // dangling_checker.cpp's mirrored field-store recognition for the identical
    // boundary). Not recursing into if/while/for bodies is deliberate scope, not
    // an oversight: promoting a param whose storage only happens on some paths
    // would incorrectly promise sharing even on paths where the store never runs.
    for (const auto& stmtPtr : body) {
        auto assign = dynamic_cast<const AssignmentStatement*>(stmtPtr.get());
        if (assign == nullptr) continue;

        std::string fieldName;
        if (!assign->target) {
            fieldName = assign->variable;  // bare identifier, implicit `this`
        } else if (auto member = dynamic_cast<const MemberAccess*>(assign->target.get())) {
            fieldName = member->member;
        } else {
            continue;
        }

        auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
        if (rhsId == nullptr) continue;

        // RHS must be exactly one of this body's own parameters.
        const bool rhsIsParam = std::any_of(params.begin(), params.end(),
            [&](const auto& p) { return p.second == rhsId->name; });
        if (!rhsIsParam) continue;

        // The field's own declared type (registered by analyzeVariableDeclaration
        // when the class body was first walked) tells us which (class, name) pair
        // to ask getPointerType about.
        const auto fieldTypeIt = symbolTypesByScope.find(makeSymbolKey(className, fieldName));
        if (fieldTypeIt == symbolTypesByScope.end()) continue;

        auto paramTypeIt = std::find_if(params.begin(), params.end(),
            [&](const auto& p) { return p.second == rhsId->name; });
        if (paramTypeIt == params.end()) continue;
        if (classNames.find(paramTypeIt->first) == classNames.end()) continue;

        if (getPointerType(fieldTypeIt->second, fieldName) != PointerType::SHARED) {
            // The field was NOT already decided SHARED by anything -- most
            // commonly `secondPassDeterminePointerTypes`' own base case
            // (`PointerType::UNIQUE`, "default unique ownership", when no
            // other signal applies). Left alone, this is a real, general,
            // pre-existing mismatch (confirmed via probe, no closures
            // involved: `class Holder { Animal stored; Holder(Animal c) {
            // stored = c; } }` called as `new Holder(new Dog())`) --
            // AstEmitter's own inline-`new`-as-call-argument path
            // (`emitMovedArg`'s NewExpression branch -> factoryFor) has NO
            // signal from THIS pass to consult and falls back to
            // `getPointerType`'s own "not found" default, which is
            // UNCONDITIONALLY SHARED (see that function's own two identical
            // `return PointerType::SHARED;` branches) -- so a constructor
            // call site passing a freshly-`new`'d, unnamed argument always
            // produces a `shared_ptr`, regardless of what this field ends up
            // deciding. A field left at UNIQUE while its own constructor
            // argument is unconditionally emitted as SHARED is a silent
            // `std::unique_ptr<T> stored(std::shared_ptr<T>)` type mismatch
            // -- a real compile error in the generated C++, not a safety
            // hole (CeeLing's own analysis never runs on already-broken
            // output), but exactly the "compiles this pass, fails the next"
            // gap the safety suite's compile-gate exists to catch.
            //
            // Fix: when nothing else drives this field's decision, make it
            // agree with the SAME default a fresh inline-constructed
            // argument already gets, rather than leaving it on
            // secondPass's unrelated "default unique ownership" base case,
            // which was never reasoning about constructor-argument
            // construction sites at all.
            // NOTE: `pointerDecisions` has an entry for EVERY variableReferences
            // key unconditionally (secondPassDeterminePointerTypes's own loop
            // writes one on every iteration, including its UNIQUE base case) --
            // so "does an entry exist" is always true and cannot distinguish
            // "a real signal fired" from "nothing did." The reason STRING is
            // the only thing that distinguishes them: the base case's literal
            // reason is "default unique ownership" (see that function's own
            // `std::string reason = "default unique ownership";`); anything
            // else means some real rule (explicit hint, back-reference,
            // bidirectional, shared-root, data-flow) already decided this
            // field and must not be silently overridden here.
            bool fieldHasOwnSignal = false;
            for (const auto& [key, ref] : variableReferences) {
                if (ref.className == fieldTypeIt->second && ref.variableName == fieldName) {
                    auto reasonIt = pointerReasons.find(key);
                    if (reasonIt != pointerReasons.end() && reasonIt->second != "default unique ownership") {
                        fieldHasOwnSignal = true;
                        break;
                    }
                }
            }
            if (fieldHasOwnSignal) continue;

            for (auto& [key, ref] : variableReferences) {
                if (ref.className == fieldTypeIt->second && ref.variableName == fieldName) {
                    pointerDecisions[key] = PointerType::SHARED;
                    pointerReasons[key] =
                        "field is populated from a stored constructor parameter, matching "
                        "the SHARED default a fresh inline-constructed argument already gets";
                    ref.reason = pointerReasons[key];
                }
            }
        }

        // The parameter's own type tells us which (class, name) key to promote --
        // registerParameters already gave every object-typed parameter a bare
        // variableReferences entry to promote here.
        for (auto& [key, ref] : variableReferences) {
            if (ref.className == paramTypeIt->first && ref.variableName == rhsId->name) {
                pointerDecisions[key] = PointerType::SHARED;
                pointerReasons[key] = "parameter is stored into an already-shared field";
                ref.reason = pointerReasons[key];
            }
        }
    }
}

namespace {
// Local, transient lookup built once per promoteSharedCallArguments call: resolves
// a callee (free function name, or "ClassName::methodName" for a method/
// constructor) to its declared parameter list. Kept local rather than permanent
// analyzer state -- nothing else in SmartPointerAnalyzer needs a general call-
// target-to-parameters map, this pass is the only consumer.
struct CalleeParams {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> freeFunctions;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> methods;      // "Class::method"
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> constructors; // "Class"
};

CalleeParams collectCalleeParams(const Program& program) {
    CalleeParams out;
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            out.freeFunctions[fn->name] = fn->parameters;
            // Dual-register under the QUALIFIED name too ("YAML::Clone") -- a
            // call site OUTSIDE the namespace spells it qualified
            // (FunctionCall::function_name is captured verbatim from source
            // text, see ASTBuilder::buildFunctionCall), so a lookup keyed only
            // by the bare name misses every external caller. Same bug class
            // (and same fix shape) as AstEmitter::collectClassNames's
            // functionParams_/functionReturnType_ dual-registration for
            // YAML::Load.
            if (!fn->namespaceName.empty()) {
                out.freeFunctions[fn->namespaceName + "::" + fn->name] = fn->parameters;
            }
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    // A specialization is excluded: out.methods is keyed by
                    // BARE "Class::method", with no notion of
                    // specializationKey, and is consulted for an ORDINARY
                    // (non-templated) call's parameter list -- a plain call
                    // can never actually resolve to a specialization's own
                    // body anyway (that requires an explicit <Type> type
                    // argument), so a specialization's parameter list has
                    // nothing legitimate to contribute here, and would only
                    // ever risk silently overwriting the PRIMARY's own
                    // (possibly different-shaped) entry -- same bug class as
                    // inferAliasReturningMethods/promoteReturnsOfSharedLocals's
                    // identical exclusion, see their comments for the
                    // confirmed real-world collision this guards against.
                    if (!method->specializationArg.empty()) continue;
                    out.methods[cls->name + "::" + method->name] = method->parameters;
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    out.constructors[cls->name] = ctor->parameters;
                }
            }
        }
    }
    return out;
}
} // namespace

void SmartPointerAnalyzer::promoteSharedCallArguments(const Program& program) {
    const CalleeParams calleeParams = collectCalleeParams(program);
    // The precise scope (free function name, "main", or "Class::method") of
    // whatever body visitStmt/visitExpr are currently walking -- set at each
    // of the 4 outer entry points below, read by every symbolTypesByScope
    // lookup in this function in PREFERENCE to the kGlobalScope fallback
    // (see registerSymbolType: every local is ALSO registered under
    // kGlobalScope as a last-write-wins fallback, which two unrelated
    // functions declaring a same-named local of DIFFERENT types silently
    // collide in -- confirmed as a real bug via probe: yaml_parser.clg's
    // `Load`'s own `Parser p` was corrupted by an unrelated `Point p` in a
    // consumer's main()). Trying the precise scope first eliminates that
    // collision without touching registerSymbolType's fallback (other,
    // lower-risk call sites may still legitimately rely on the fallback for
    // a variable whose exact scope isn't threaded through to them).
    std::string currentScope;

    // A free function literally named `begin`/`end` taking a single
    // class-typed parameter exists, BY CONSTRUCTION, to support ADL-found
    // range-for from OTHER .clg files (Step 27's mechanism -- ordinary
    // member begin()/end() can't be used when the iterator and the
    // container have a circular type dependency CeeLing cannot express).
    // The pre-existing forcing further down only fires when a `for`
    // statement iterating this exact class appears in the SAME transpile
    // unit as begin/end's own definition -- but CeeLing compiles each .clg
    // file independently (real separate compilation, not a text merge), so
    // `yaml_node.clg`'s own standalone transpile (defining begin/end) never
    // sees ANY for-loop; the for-loop lives in a driver .clg that only
    // #includes yaml_node.clh, transpiled as a SEPARATE unit that cannot
    // retroactively change yaml_node.h's already-emitted signature.
    // Confirmed as a real, reproducible cross-file gap (not new to RB18,
    // though RB18's changes newly exposed it): forced here UNCONDITIONALLY,
    // not merely when local usage evidence happens to agree, since a
    // function named begin/end taking one class parameter has no other
    // legitimate purpose in this codebase.
    for (const char* fnName : {"begin", "end"}) {
        const auto paramsIt = calleeParams.freeFunctions.find(fnName);
        if (paramsIt == calleeParams.freeFunctions.end() || paramsIt->second.size() != 1) continue;
        const auto& [paramType, paramName] = paramsIt->second[0];
        if (classNames.find(paramType) == classNames.end()) continue;
        const std::string paramKey = buildKey(fnName, paramType, paramName);
        pointerDecisions[paramKey] = PointerType::SHARED;
        pointerReasons[paramKey] =
            "begin()/end() parameter for ADL range-for support (by-value, must accept a shared_ptr caller "
            "argument from any .clg file, not just one with a for-loop in the same transpile unit)";
    }

    // A class method literally named `operator==`/`operator!=` whose single
    // parameter is the class itself exists to support comparison call sites
    // ANYWHERE that class is compared (AstEmitter routes `a == b` through it
    // whenever both operands are that class -- see the BinaryExpression
    // handling in ast_emitter.cpp's emitExpression). Forced unconditionally,
    // same reasoning as begin/end above: a caller
    // comparing two `Node`-typed locals ALWAYS passes a shared_ptr argument
    // (every Node local defaults to shared_ptr unless something ELSE forces
    // unique), so the comparison operator's own parameter must accept
    // shared_ptr regardless of whether local usage evidence WITHIN the
    // declaring class's own file happens to agree -- confirmed as a real
    // compile failure via Iterator::operator!=(Iterator other)'s own body
    // (`current != other.current`, both Node-typed shared_ptr fields),
    // which never independently forces Node::operator!='s parameter shared.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr) continue;
            if (method->name != "operator==" && method->name != "operator!=") continue;
            if (method->parameters.size() != 1 || method->parameters[0].first != cls->name) continue;
            const std::string methodScope = cls->name + "::" + method->name;
            const std::string paramKey =
                buildKey(methodScope, method->parameters[0].first, method->parameters[0].second);
            pointerDecisions[paramKey] = PointerType::SHARED;
            pointerReasons[paramKey] =
                "operator==/!= comparison parameter (must accept a shared_ptr caller argument from any "
                "comparison call site, not just one with matching local usage evidence in this file)";
        }
    }

    // Same cross-file gap, for every free function parameter already
    // classified read-only (classifyReadOnlyFreeFunctionParams, Step 29):
    // the ordinary per-call-site forcing further down (`isReadOnlyFreeFunctionParam`
    // branch) only fires for a call site actually present in THIS transpile
    // unit -- a read-only function like `Clone(const Node&)` defined in
    // yaml_node.clg but only ever CALLED from a separate driver .clg never
    // gets its own parameter forced shared when yaml_node.clg is transpiled
    // standalone (confirmed via probe: `Clone`'s parameter stayed
    // unique_ptr in yaml_node.h, so a caller's shared_ptr argument from
    // another file failed to bind). A read-only classification is a
    // property of the FUNCTION's OWN body, independent of any particular
    // call site, so it can and should be forced unconditionally here too.
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        for (const auto& [paramType, paramName] : fn->parameters) {
            if (classNames.find(paramType) == classNames.end()) continue;
            if (!isReadOnlyFreeFunctionParam(fn->name, paramName)) continue;
            const std::string paramKey = buildKey(fn->name, paramType, paramName);
            pointerDecisions[paramKey] = PointerType::SHARED;
            pointerReasons[paramKey] =
                "read-only free-function parameter (forced regardless of call sites in this transpile unit -- "
                "a caller in ANY .clg file needs a shared_ptr to bind to)";
        }
    }

    // "ClassName" -> the parameter name of that class's HANDLE-REBIND
    // `operator=(SameClass other)` overload, if declared. Built separately
    // from calleeParams.methods (keyed "Class::operator=", collapsed to ONE
    // entry per class) because a class declaring BOTH `operator=(string)`
    // and `operator=(SameClass)` (RB18's yaml-cling Node does exactly this)
    // has two arity-1 overloads that silently overwrite each other under
    // that collapsed key -- confirmed via probe: whichever overload is
    // declared LAST in source order wins, so `calleeParams.methods.find(
    // "Node::operator=")` could resolve to the STRING overload's params
    // instead of the Node one, silently defeating the forcing below. Mirrors
    // AstEmitter::operatorAssignParamTypes_'s identical fix for the same
    // root cause on the emitter side.
    std::map<std::string, std::string> handleRebindAssignParamName;
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr || method->name != "operator=" || method->parameters.size() != 1) continue;
            if (method->parameters[0].first == cls->name) {
                handleRebindAssignParamName[cls->name] = method->parameters[0].second;
            }
        }
    }

    // Resolve a call/constructor expression's target parameter list, then promote
    // every argument bound to an already-SHARED parameter to SHARED itself --
    // otherwise the caller's own local stays UNIQUE and the promotion sub-pass 1
    // just did is invisible at the call site (AstEmitter would still emit
    // unique_ptr for the caller's variable). Argument resolution is deliberately
    // lightweight (bare identifier only, via symbolTypesByScope's global-scope
    // fallback), matching this codebase's existing "not full type inference"
    // conservatism elsewhere (e.g. DanglingChecker::asObjectLocalName).
    auto promoteArgsAgainstParams = [this, &currentScope](
        const std::vector<ExpressionPtr>& args,
        const std::vector<std::pair<std::string, std::string>>& params) {
        for (size_t i = 0; i < args.size() && i < params.size(); ++i) {
            const auto& [paramType, paramName] = params[i];
            if (classNames.find(paramType) == classNames.end()) continue;
            if (getPointerType(paramType, paramName) != PointerType::SHARED) continue;

            auto argId = dynamic_cast<const Identifier*>(args[i].get());
            if (argId == nullptr) continue;
            auto argTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                : symbolTypesByScope.find(makeSymbolKey(currentScope, argId->name));
            if (argTypeIt == symbolTypesByScope.end()) {
                argTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, argId->name));
            }
            if (argTypeIt == symbolTypesByScope.end()) continue;

            for (auto& [key, ref] : variableReferences) {
                if (ref.className == argTypeIt->second && ref.variableName == argId->name) {
                    pointerDecisions[key] = PointerType::SHARED;
                    pointerReasons[key] = "passed to a parameter that is stored into an already-shared field";
                    ref.reason = pointerReasons[key];
                }
            }
        }
    };

    std::function<void(const Expression*)> visitExpr = [&](const Expression* expr) {
        if (expr == nullptr) return;
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            for (const auto& arg : call->arguments) visitExpr(arg.get());
            if (call->callee) {
                // `recv.method(args)` -- resolve recv's class, then the class's
                // method's parameter list.
                if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                    if (auto recvId = dynamic_cast<const Identifier*>(member->object.get())) {
                        auto recvTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                            : symbolTypesByScope.find(makeSymbolKey(currentScope, recvId->name));
                        if (recvTypeIt == symbolTypesByScope.end()) {
                            recvTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, recvId->name));
                        }
                        if (recvTypeIt != symbolTypesByScope.end()) {
                            const auto paramsIt = calleeParams.methods.find(
                                recvTypeIt->second + "::" + member->member);
                            if (paramsIt != calleeParams.methods.end()) {
                                promoteArgsAgainstParams(call->arguments, paramsIt->second);
                            }
                        }
                    }
                }
            } else {
                // Plain call `f(args)`.
                const auto paramsIt = calleeParams.freeFunctions.find(call->function_name);
                if (paramsIt != calleeParams.freeFunctions.end()) {
                    promoteArgsAgainstParams(call->arguments, paramsIt->second);

                    // A parameter classified read-only (classifyReadOnlyFreeFunctionParams
                    // -- see isReadOnlyFreeFunctionParam) must be able to receive the
                    // caller's argument WITHOUT consuming it (yaml-cpp API-compatibility:
                    // a `const Node&`-shaped function like Clone must not move its
                    // argument). Both the callee's OWN parameter and the caller's
                    // argument are forced SHARED here -- unconditionally, not merely
                    // "if already shared" -- for the exact same reason Step 27's ADL
                    // begin()/end() forcing needed both sides forced: the parameter and
                    // the argument must be the SAME C++ type for the call to compile at
                    // all (a shared_ptr argument cannot bind to a unique_ptr parameter),
                    // and a fresh local's own default (unique) has no reason to already
                    // agree with a parameter's default (shared) by coincidence.
                    // buildKey needs the callee's BARE name (matching how
                    // registerParameters/analyzeStatement scope a function
                    // DEFINITION's own parameters -- always funcDecl->name,
                    // never qualified, even for a namespaced function) --
                    // call->function_name may be qualified ("YAML::Clone") when
                    // called from outside the namespace, so strip it the same
                    // way Step 27's ADL-forcing code strips a qualified class
                    // name for the same reason.
                    const size_t fnSepPos = call->function_name.rfind("::");
                    const std::string calleeBareName = fnSepPos == std::string::npos
                        ? call->function_name : call->function_name.substr(fnSepPos + 2);
                    for (size_t i = 0; i < call->arguments.size() && i < paramsIt->second.size(); ++i) {
                        const auto& [paramType, paramName] = paramsIt->second[i];
                        if (!isReadOnlyFreeFunctionParam(call->function_name, paramName)) continue;

                        const std::string paramKey = buildKey(calleeBareName, paramType, paramName);
                        pointerDecisions[paramKey] = PointerType::SHARED;
                        pointerReasons[paramKey] =
                            "read-only free-function parameter (yaml-cpp API-compatibility: "
                            "must not consume the caller's argument, e.g. Clone(const Node&))";

                        auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                        if (argId == nullptr) continue;
                        auto argTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                            : symbolTypesByScope.find(makeSymbolKey(currentScope, argId->name));
                        if (argTypeIt == symbolTypesByScope.end()) {
                            argTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, argId->name));
                        }
                        if (argTypeIt == symbolTypesByScope.end()) continue;
                        for (auto& [key, ref] : variableReferences) {
                            if (ref.className == argTypeIt->second && ref.variableName == argId->name) {
                                pointerDecisions[key] = PointerType::SHARED;
                                pointerReasons[key] =
                                    "passed to a read-only free-function parameter (must survive the call)";
                                ref.reason = pointerReasons[key];
                            }
                        }
                    }
                }
            }
        } else if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
            for (const auto& arg : neww->arguments) visitExpr(arg.get());
            const auto paramsIt = calleeParams.constructors.find(neww->className);
            if (paramsIt != calleeParams.constructors.end()) {
                promoteArgsAgainstParams(neww->arguments, paramsIt->second);
            }
        } else if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            visitExpr(bin->left.get());
            visitExpr(bin->right.get());
            // `out << x` (stream chain) resolves to a user-defined free
            // `operator<<(ostream, T)` via ORDINARY C++ operator overload
            // resolution -- this is NOT a FunctionCall AST node at all (the
            // grammar/AST model `<<` uniformly as a BinaryExpression, the same
            // node std::cout << x already used before any user operator<<
            // existed), so the free-function call-site forcing above never
            // sees it. Without this, a class-typed right operand streamed via
            // `<<` into a read-only `operator<<` parameter fails to compile
            // the exact same way an ordinary call would without Step 29's
            // forcing (confirmed via probe: std::unique_ptr's deleted copy
            // constructor rejects the implicit by-value binding). Only the
            // RIGHT operand can ever be the class-typed value being streamed
            // (the left is always the stream itself); a bare Identifier is
            // required here (mirrors promoteArgsAgainstParams's own
            // resolution granularity).
            if (bin->operator_str == "<<") {
                const auto opParamsIt = calleeParams.freeFunctions.find("operator<<");
                if (opParamsIt != calleeParams.freeFunctions.end() && opParamsIt->second.size() == 2) {
                    const auto& [paramType, paramName] = opParamsIt->second[1];
                    if (classNames.find(paramType) != classNames.end() &&
                        isReadOnlyFreeFunctionParam("operator<<", paramName)) {
                        if (auto rhsId = dynamic_cast<const Identifier*>(bin->right.get())) {
                            auto rhsTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                                : symbolTypesByScope.find(makeSymbolKey(currentScope, rhsId->name));
                            if (rhsTypeIt == symbolTypesByScope.end()) {
                                rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, rhsId->name));
                            }
                            // Strip a qualified rhs type ("YAML::Node", as a
                            // driver OUTSIDE the namespace spells it) down to
                            // its simple name before comparing against
                            // operator<<'s own bare paramType ("Node", as
                            // declared inside the namespace) -- the same
                            // qualified-vs-bare mismatch class found and fixed
                            // repeatedly this session (Step 27's ADL forcing,
                            // Step 29's Clone forcing).
                            std::string rhsSimpleType;
                            if (rhsTypeIt != symbolTypesByScope.end()) {
                                const size_t sepPos = rhsTypeIt->second.rfind("::");
                                rhsSimpleType = sepPos == std::string::npos
                                    ? rhsTypeIt->second : rhsTypeIt->second.substr(sepPos + 2);
                            }
                            if (rhsTypeIt != symbolTypesByScope.end() && rhsSimpleType == paramType) {
                                const std::string paramKey = buildKey("operator<<", paramType, paramName);
                                pointerDecisions[paramKey] = PointerType::SHARED;
                                pointerReasons[paramKey] =
                                    "read-only operator<< parameter (must not consume the streamed value)";
                                for (auto& [key, ref] : variableReferences) {
                                    if (ref.className == rhsTypeIt->second && ref.variableName == rhsId->name) {
                                        pointerDecisions[key] = PointerType::SHARED;
                                        pointerReasons[key] = "streamed via operator<< (must survive the call)";
                                        ref.reason = pointerReasons[key];
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // `a == b`/`a != b` where both operands are the same class and
            // that class declares its own operator==/!=: AstEmitter routes
            // this through `a->operator==(b)` (see emitExpression's
            // BinaryExpression handling) -- again NOT a FunctionCall AST
            // node, so invisible to the ordinary call-site forcing above.
            // The method's OWN parameter is already forced SHARED
            // unconditionally (see the begin/end-style forcing near the top
            // of this function), but the CALLER's argument (the right
            // operand) still needs forcing too, or a caller whose local
            // independently defaulted to UNIQUE (ordinary usage-based
            // inference, e.g. a single-owner `Node other = new Node(...);`
            // never shared elsewhere) fails to bind -- confirmed via a real
            // compile error: `root->operator==(other)` needs a shared_ptr
            // argument, `other` defaulted to unique_ptr.
            if ((bin->operator_str == "==" || bin->operator_str == "!=")) {
                if (auto rhsId = dynamic_cast<const Identifier*>(bin->right.get())) {
                    auto rhsTypeIt = currentScope.empty() ? symbolTypesByScope.end()
                        : symbolTypesByScope.find(makeSymbolKey(currentScope, rhsId->name));
                    if (rhsTypeIt == symbolTypesByScope.end()) {
                        rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, rhsId->name));
                    }
                    if (rhsTypeIt != symbolTypesByScope.end() && classNames.count(rhsTypeIt->second) > 0) {
                        const size_t sepPos = rhsTypeIt->second.rfind("::");
                        const std::string rhsSimpleType =
                            sepPos == std::string::npos ? rhsTypeIt->second : rhsTypeIt->second.substr(sepPos + 2);
                        const std::string methodName = "operator" + bin->operator_str;
                        const auto opParamsIt = calleeParams.methods.find(rhsSimpleType + "::" + methodName);
                        if (opParamsIt != calleeParams.methods.end() && opParamsIt->second.size() == 1 &&
                            opParamsIt->second[0].first == rhsSimpleType) {
                            for (auto& [key, ref] : variableReferences) {
                                if (ref.className == rhsTypeIt->second && ref.variableName == rhsId->name) {
                                    pointerDecisions[key] = PointerType::SHARED;
                                    pointerReasons[key] = "passed as the right operand of a class-typed ==/!= "
                                                           "comparison (must bind to the shared_ptr parameter)";
                                    ref.reason = pointerReasons[key];
                                }
                            }
                        }
                    }
                }
            }
        } else if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            visitExpr(un->operand.get());
        } else if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
            visitExpr(member->object.get());
        } else if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            visitExpr(sub->object.get());
            visitExpr(sub->index.get());
        }
    };

    std::function<void(const Statement*)> visitStmt = [&](const Statement* stmt) {
        if (stmt == nullptr) return;
        if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            visitExpr(exprStmt->expression.get());
        } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            visitExpr(decl->expression.get());
        } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
            visitExpr(assign->expression.get());
            // Same-class `n = otherNode;` handle-rebind forcing lives in the
            // separate forceHandleRebindAssignments walk below (needs class/
            // method SCOPE context -- whether `n` is a read-only PARAMETER,
            // not just any same-class local -- that this generic visitStmt
            // does not track; see that walk's own header comment for why the
            // gate is required: an ORDINARY loop-advancement local like
            // yaml_emitter.clg's `cur = cur.nextSibling();` has the exact
            // same AST shape but needs the OPPOSITE treatment, a plain
            // repoint, not a forced-shared handle-rebind -- confirmed via a
            // real segfault before this gate was added).
        } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            visitExpr(ret->expression.get());
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
            visitExpr(ifs->condition.get());
            for (const auto& inner : ifs->body) visitStmt(inner.get());
            for (const auto& inner : ifs->elseBody) visitStmt(inner.get());
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
            visitExpr(whiles->condition.get());
            for (const auto& inner : whiles->body) visitStmt(inner.get());
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
            visitExpr(fors->container.get());
            // Range-for over a class-typed container relying on ADL-found free
            // functions (no MEMBER begin() -- see AstEmitter's matching
            // hasMemberBegin check) implicitly calls `begin(container)` AND
            // `end(container)` in the generated C++ -- pure C++-compiler
            // desugaring, invisible to the ordinary promoteArgsAgainstParams walk
            // below (which only visits REAL call/constructor expressions in the
            // AST). Both the container variable and BOTH free functions'
            // parameters are forced to SHARED here, UNCONDITIONALLY (not merely
            // matched to whatever the independent usage-based inference already
            // decided) -- confirmed via direct probe that unique_ptr cannot work
            // for this pattern AT ALL, regardless of matching: CeeLing has no
            // reference-parameter syntax, so `begin`/`end` always take the
            // container BY VALUE, and range-for needs the container passed to
            // BOTH calls -- but unique_ptr's copy constructor is deleted, so
            // passing a unique_ptr-backed container into two separate by-value
            // parameters can never compile no matter what kind either side
            // independently infers. shared_ptr's cheap, safe copy (refcount
            // bump) is the only kind that can satisfy "passed by value twice".
            // Mirrors promoteArgsAgainstParams's own bare-identifier-only
            // resolution (this pass's established conservatism).
            if (auto containerId = dynamic_cast<const Identifier*>(fors->container.get())) {
                auto classIt = currentScope.empty() ? symbolTypesByScope.end()
                    : symbolTypesByScope.find(makeSymbolKey(currentScope, containerId->name));
                if (classIt == symbolTypesByScope.end()) {
                    classIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, containerId->name));
                }
                if (classIt != symbolTypesByScope.end()) {
                    const std::string& containerClass = classIt->second;
                    // The container variable's declared type may be qualified
                    // ("YAML::Node", as the source wrote it) while a free
                    // function's parameter declared INSIDE that namespace is
                    // bare ("Node") -- strip to the simple name for comparison
                    // (the same qualified-vs-bare mismatch class found and fixed
                    // elsewhere this session for namespaced types).
                    const size_t sepPos = containerClass.rfind("::");
                    const std::string containerSimpleClass =
                        sepPos == std::string::npos ? containerClass : containerClass.substr(sepPos + 2);
                    const bool hasMemberBegin =
                        calleeParams.methods.find(containerClass + "::begin") != calleeParams.methods.end() ||
                        calleeParams.methods.find(containerSimpleClass + "::begin") != calleeParams.methods.end();
                    if (!hasMemberBegin && classNames.find(containerClass) != classNames.end()) {
                        const bool hasFreeBegin = calleeParams.freeFunctions.count("begin") > 0;
                        const bool hasFreeEnd = calleeParams.freeFunctions.count("end") > 0;
                        if (hasFreeBegin && hasFreeEnd) {
                            // Force the container variable itself.
                            for (auto& [key, ref] : variableReferences) {
                                if (ref.className == containerClass && ref.variableName == containerId->name) {
                                    pointerDecisions[key] = PointerType::SHARED;
                                    pointerReasons[key] =
                                        "used as a range-for container whose iteration relies on ADL-found free begin()/end() (by-value parameters, unique_ptr cannot work)";
                                    ref.reason = pointerReasons[key];
                                }
                            }
                            // Force `begin`'s and `end`'s own first parameter.
                            for (const char* fnName : {"begin", "end"}) {
                                const auto& params = calleeParams.freeFunctions.at(fnName);
                                if (params.empty()) continue;
                                const auto& [paramType, paramName] = params[0];
                                if (paramType != containerClass && paramType != containerSimpleClass) continue;
                                const std::string paramKey = buildKey(fnName, paramType, paramName);
                                pointerDecisions[paramKey] = PointerType::SHARED;
                                pointerReasons[paramKey] =
                                    "range-for container parameter for ADL-found free begin()/end() (by-value, unique_ptr cannot work)";
                            }
                        }
                    }
                }
            }
            for (const auto& inner : fors->body) visitStmt(inner.get());
        } else if (auto tryStmt = dynamic_cast<const TryStatement*>(stmt)) {
            for (const auto& inner : tryStmt->body) visitStmt(inner.get());
            for (const auto& inner : tryStmt->catchBody) visitStmt(inner.get());
        } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
            visitExpr(throwStmt->expression.get());
        }
    };

    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            currentScope = fn->name;
            for (const auto& inner : fn->body) visitStmt(inner.get());
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            currentScope = "main";
            for (const auto& inner : mn->body) visitStmt(inner.get());
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    currentScope = cls->name + "::" + method->name;
                    for (const auto& inner : method->body) visitStmt(inner.get());
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    currentScope = cls->name + "::" + ctor->name;
                    for (const auto& inner : ctor->body) visitStmt(inner.get());
                }
            }
        }
    }

    // A second, separate walk specifically for IMPLICIT-THIS calls to a
    // read-only-classified SIBLING METHOD (`parseBlockMapInto(target, ...)`
    // inside another Parser method -- call->callee is null, same AST shape as
    // a free-function call, so the walk above's "Plain call f(args)" branch
    // never matches it: calleeParams.freeFunctions has no entry for a method,
    // which is only ever registered under "Class::method"). Kept as an
    // independent pass (not folded into visitExpr/visitStmt above) rather than
    // threading a `currentClass` parameter through that entire, already-large
    // recursive walker -- this only needs to look at DIRECT calls in a
    // method's own body, not descend into nested lambdas/closures (CeeLing
    // has none), so a fresh, method-scoped recursive walk is simpler than
    // retrofitting class-context onto every existing branch. Mirrors the
    // free-function forcing above exactly, just keyed by (class, method).
    std::function<void(const Expression*, const std::string&, const std::string&)> forceReadOnlyMethodArg =
        [&](const Expression* expr, const std::string& currentClass, const std::string& currentMethod) {
            if (expr == nullptr) return;
            if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
                for (const auto& arg : call->arguments) forceReadOnlyMethodArg(arg.get(), currentClass, currentMethod);
                if (!call->callee) {
                    const auto paramsIt = calleeParams.methods.find(currentClass + "::" + call->function_name);
                    if (paramsIt != calleeParams.methods.end()) {
                        for (size_t i = 0; i < call->arguments.size() && i < paramsIt->second.size(); ++i) {
                            const auto& [paramType, paramName] = paramsIt->second[i];
                            if (!isReadOnlyMethodParam(currentClass, call->function_name, paramName)) continue;

                            const std::string paramKey =
                                buildKey(currentClass + "::" + call->function_name, paramType, paramName);
                            pointerDecisions[paramKey] = PointerType::SHARED;
                            pointerReasons[paramKey] =
                                "read-only method parameter (mutated in place across a same-class call chain, "
                                "e.g. Parser::parseValueInto's `target` -- must not consume the caller's argument)";

                            auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                            if (argId == nullptr) continue;
                            const auto argTypeIt = symbolTypesByScope.find(makeSymbolKey(currentMethod, argId->name));
                            const auto argTypeItGlobal =
                                symbolTypesByScope.find(makeSymbolKey(kGlobalScope, argId->name));
                            const std::string argType = argTypeIt != symbolTypesByScope.end() ? argTypeIt->second
                                : (argTypeItGlobal != symbolTypesByScope.end() ? argTypeItGlobal->second : "");
                            if (argType.empty()) continue;
                            for (auto& [key, ref] : variableReferences) {
                                if (ref.className == argType && ref.variableName == argId->name) {
                                    pointerDecisions[key] = PointerType::SHARED;
                                    pointerReasons[key] =
                                        "passed to a read-only method parameter (must survive the call)";
                                    ref.reason = pointerReasons[key];
                                }
                            }
                        }
                    }
                }
                if (call->callee) forceReadOnlyMethodArg(call->callee.get(), currentClass, currentMethod);
                return;
            }
            if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
                forceReadOnlyMethodArg(bin->left.get(), currentClass, currentMethod);
                forceReadOnlyMethodArg(bin->right.get(), currentClass, currentMethod);
            } else if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
                forceReadOnlyMethodArg(un->operand.get(), currentClass, currentMethod);
            } else if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
                forceReadOnlyMethodArg(member->object.get(), currentClass, currentMethod);
            } else if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
                forceReadOnlyMethodArg(sub->object.get(), currentClass, currentMethod);
                forceReadOnlyMethodArg(sub->index.get(), currentClass, currentMethod);
            }
        };
    std::function<void(const std::vector<StatementPtr>&, const std::string&, const std::string&)>
        forceReadOnlyMethodArgsInBody = [&](const std::vector<StatementPtr>& body, const std::string& currentClass,
                                             const std::string& currentMethod) {
            for (const auto& stmtPtr : body) {
                const Statement* stmt = stmtPtr.get();
                if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                    forceReadOnlyMethodArg(exprStmt->expression.get(), currentClass, currentMethod);
                } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                    forceReadOnlyMethodArg(decl->expression.get(), currentClass, currentMethod);
                } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                    forceReadOnlyMethodArg(assign->expression.get(), currentClass, currentMethod);
                } else if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                    forceReadOnlyMethodArg(ret->expression.get(), currentClass, currentMethod);
                } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                    forceReadOnlyMethodArg(ifs->condition.get(), currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(ifs->body, currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(ifs->elseBody, currentClass, currentMethod);
                } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                    forceReadOnlyMethodArg(whiles->condition.get(), currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(whiles->body, currentClass, currentMethod);
                } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                    forceReadOnlyMethodArgsInBody(fors->body, currentClass, currentMethod);
                } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                    forceReadOnlyMethodArgsInBody(trys->body, currentClass, currentMethod);
                    forceReadOnlyMethodArgsInBody(trys->catchBody, currentClass, currentMethod);
                }
            }
        };
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr) continue;
            forceReadOnlyMethodArgsInBody(method->body, cls->name, cls->name + "::" + method->name);
        }
    }

    // `n = otherNode;` handle-rebind forcing (see AstEmitter's matching
    // AssignmentStatement gate for the full rationale): the emitter only
    // routes a same-class assignment through the user's `operator=` when the
    // LHS is a PARAMETER already classified read-only (isReadOnlyMethodParam/
    // isReadOnlyFreeFunctionParam) -- an ordinary local (e.g. yaml_emitter.
    // clg's `cur = cur.nextSibling();`, a plain loop-advancement repoint) must
    // NOT be forced shared here, or the SAME segfault that gate was added to
    // fix reappears (this pass forcing sharedness independently of what the
    // emitter will actually route through operator=). Scoped per (class,
    // method) so `isReadOnlyMethodParam`/`isReadOnlyFreeFunctionParam` can be
    // queried correctly -- mirrors forceReadOnlyMethodArgsInBody's own
    // class/method-scoped walk immediately above, kept as a separate pass
    // since it triggers on AssignmentStatement, not FunctionCall.
    std::function<void(const std::vector<StatementPtr>&, const std::string&, const std::string&)>
        forceHandleRebindAssignmentsInBody = [&](const std::vector<StatementPtr>& body,
                                                  const std::string& currentClass,
                                                  const std::string& currentFunctionName) {
            for (const auto& stmtPtr : body) {
                const Statement* stmt = stmtPtr.get();
                if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                    auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
                    if (rhsId == nullptr) continue;
                    // currentFunctionName is precisely scoped by the callers
                    // below ("Class::method" for a method, bare name for a
                    // free function -- matching registerSymbolType's own
                    // registration format), so it must be tried BEFORE the
                    // kGlobalScope fallback -- same collision class as every
                    // other site fixed in this function (a same-named local
                    // of a different type elsewhere in the program silently
                    // wins the global fallback's last-write-wins entry).
                    const std::string preciseScope = currentClass.empty()
                        ? currentFunctionName : currentClass + "::" + currentFunctionName;
                    auto lhsTypeIt = symbolTypesByScope.find(makeSymbolKey(preciseScope, assign->variable));
                    if (lhsTypeIt == symbolTypesByScope.end()) {
                        lhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, assign->variable));
                    }
                    auto rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(preciseScope, rhsId->name));
                    if (rhsTypeIt == symbolTypesByScope.end()) {
                        rhsTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, rhsId->name));
                    }
                    if (lhsTypeIt == symbolTypesByScope.end() || rhsTypeIt == symbolTypesByScope.end() ||
                        lhsTypeIt->second != rhsTypeIt->second ||
                        classNames.find(lhsTypeIt->second) == classNames.end()) {
                        continue;
                    }
                    const auto paramNameIt = handleRebindAssignParamName.find(lhsTypeIt->second);
                    if (paramNameIt == handleRebindAssignParamName.end()) continue;
                    const bool lhsIsReadOnlyParam =
                        (!currentClass.empty() &&
                         isReadOnlyMethodParam(currentClass, currentFunctionName, assign->variable)) ||
                        (currentClass.empty() && isReadOnlyFreeFunctionParam(currentFunctionName, assign->variable));
                    if (!lhsIsReadOnlyParam) continue;

                    const std::string methodScope = lhsTypeIt->second + "::operator=";
                    const std::string paramKey = buildKey(methodScope, lhsTypeIt->second, paramNameIt->second);
                    pointerDecisions[paramKey] = PointerType::SHARED;
                    pointerReasons[paramKey] =
                        "operator=(Node) handle-rebind parameter (must alias, not consume, the RHS)";
                    for (auto& [key, ref] : variableReferences) {
                        if (ref.className == rhsTypeIt->second && ref.variableName == rhsId->name) {
                            pointerDecisions[key] = PointerType::SHARED;
                            pointerReasons[key] = "passed to operator=(Node)'s handle-rebind parameter";
                            ref.reason = pointerReasons[key];
                        }
                    }
                } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(ifs->body, currentClass, currentFunctionName);
                    forceHandleRebindAssignmentsInBody(ifs->elseBody, currentClass, currentFunctionName);
                } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(whiles->body, currentClass, currentFunctionName);
                } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(fors->body, currentClass, currentFunctionName);
                } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                    forceHandleRebindAssignmentsInBody(trys->body, currentClass, currentFunctionName);
                    forceHandleRebindAssignmentsInBody(trys->catchBody, currentClass, currentFunctionName);
                }
            }
        };
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            forceHandleRebindAssignmentsInBody(fn->body, "", fn->name);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                if (method == nullptr) continue;
                forceHandleRebindAssignmentsInBody(method->body, cls->name, method->name);
            }
        }
    }
}

void SmartPointerAnalyzer::classifyValueTypeClasses(const Program& program) {
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (!cls) continue;
        // Two independent routes into the SAME valueTypeClasses_ set: the
        // pre-existing structural inference (a class declaring the full
        // operator*/operator++/operator!= iterator protocol -- the Iterator
        // case) OR an explicit `struct` declaration (ClassDeclaration::
        // isValueType, set at parse time -- see class_declaration's grammar.js
        // comment for why this needs to be an explicit opt-in, not inferred
        // from "all fields are primitives"). Every existing isValueTypeClass-
        // gated call site (local/field declarations, parameter passing, and
        // now also AstEmitter's vector-element codegen) picks up a `struct`
        // class for free via this one shared set -- no other change needed
        // for those existing sites.
        bool hasStar = false, hasIncrement = false, hasNotEqual = false;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (!method) continue;
            if (method->name == "operator*") hasStar = true;
            else if (method->name == "operator++") hasIncrement = true;
            else if (method->name == "operator!=") hasNotEqual = true;
        }
        if (cls->isValueType || (hasStar && hasIncrement && hasNotEqual)) {
            valueTypeClasses_.insert(cls->name);
        }
    }
    // EXPLORATORY Eigen spike (see library_summary.h's isKnownExternalClassType --
    // same narrow, hardcoded allow-list, not the general third-party mechanism):
    // a known-external class has no ClassDeclaration for the loop above to see at
    // all, so it needs its own explicit route into the SAME valueTypeClasses_ set.
    // This is what makes `new Eigen::MatrixXd(...)` emit as a bare, unwrapped
    // constructor call (matching real Eigen's own by-value usage) instead of
    // being wrapped in a smart pointer meant for CeeLing-owned heap objects.
    if (isKnownExternalClassType("Eigen::MatrixXd")) {
        valueTypeClasses_.insert("Eigen::MatrixXd");
    }
}

bool SmartPointerAnalyzer::isValueTypeClass(const std::string& className) const {
    return valueTypeClasses_.find(className) != valueTypeClasses_.end();
}

bool SmartPointerAnalyzer::paramUseDisqualifiesReadOnly(
    const Expression* expr, const std::string& paramName, const std::string& currentClass,
    const std::set<std::string>& okCallees) const {
    if (expr == nullptr) return false;
    // A BARE occurrence of paramName is exactly what asObjectLocalName-style
    // move detection keys on elsewhere (DanglingChecker) -- `paramName.field`
    // is a MemberAccess, a completely different expression node, and never
    // disqualifies on its own (checked by NOT recursing into a MemberAccess's
    // object here the way visitExpr elsewhere does -- see the caller, which
    // only calls this on argument/RHS positions, never descends through
    // MemberAccess itself).
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return id->name == paramName;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        return paramUseDisqualifiesReadOnly(bin->left.get(), paramName, currentClass, okCallees) ||
               paramUseDisqualifiesReadOnly(bin->right.get(), paramName, currentClass, okCallees);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        // `!paramName` (falsy/null check, e.g. `if (!node) { ... }`) reads,
        // does not consume -- explicitly non-disqualifying even when the
        // operand IS the bare parameter itself (unlike every other position,
        // where a bare paramName identifier disqualifies). A NESTED use inside
        // the operand still needs checking (e.g. `!node.child` -- though that's
        // a MemberAccess, already non-disqualifying on its own), so this still
        // recurses rather than returning false unconditionally; the fix is
        // narrowly that `!paramName` itself must not match the bare-identifier
        // rule the way `un->operand.get()` being paramName directly would.
        if (un->operator_str == "!") {
            auto operandId = dynamic_cast<const Identifier*>(un->operand.get());
            if (operandId != nullptr && operandId->name == paramName) return false;
        }
        return paramUseDisqualifiesReadOnly(un->operand.get(), paramName, currentClass, okCallees);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // A bare paramName passed as a CALL ARGUMENT disqualifies UNLESS the
        // callee's OWN parameter at that position is already known read-only
        // (`okCallees` -- a fixed point over ALL free functions AND methods,
        // mirroring inferAliasReturningMethods's own established algorithm
        // shape; see classifyReadOnlyFreeFunctionParams for the round-by-round
        // driver). Motivated by a real, confirmed gap: yaml-cling's
        // `operator<<(ostream out, Node node) { out << Dump(node); return
        // out; }` passes `node` bare into `Dump(node)` -- without this,
        // `operator<<`'s OWN parameter was wrongly disqualified even though
        // Dump only ever reads its parameter's fields, causing `std::cout <<
        // someNode;` to fail to compile (a shared_ptr couldn't bind to
        // operator<<'s unique_ptr parameter). `!call->callee` covers BOTH a
        // free-function call AND a same-class IMPLICIT-this method call
        // (`parseBlockMapInto(target, ...)` inside another Parser method --
        // indistinguishable from a free call at this AST shape); when
        // `currentClass` is non-empty, the method-scoped key is tried FIRST
        // (a same-class bare call always resolves to a sibling method, never
        // an unrelated free function of the same name), falling back to the
        // free-function key otherwise.
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
            if (argId != nullptr && argId->name == paramName && !call->callee) {
                const std::string methodKey =
                    currentClass + "::" + call->function_name + "::" + std::to_string(i);
                const std::string freeKey = call->function_name + "::" + std::to_string(i);
                if ((!currentClass.empty() && okCallees.count(methodKey) > 0) ||
                    okCallees.count(freeKey) > 0) {
                    continue;  // passed to an already-read-only param
                }
            }
            if (paramUseDisqualifiesReadOnly(call->arguments[i].get(), paramName, currentClass, okCallees)) {
                return true;
            }
        }
        if (call->callee) {
            if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                // The RECEIVER of a method call, e.g. `paramName.method()` --
                // bare paramName as receiver is a read (dispatch), not a move;
                // only descend into a nested receiver expression, never treat
                // the immediate Identifier receiver itself as disqualifying.
                if (auto recvMember = dynamic_cast<const MemberAccess*>(member->object.get())) {
                    return paramUseDisqualifiesReadOnly(recvMember, paramName, currentClass, okCallees);
                }
            }
        }
        return false;
    }
    // MemberAccess (paramName.field), SubscriptExpression, NewExpression,
    // literals: never disqualifying on their own -- MemberAccess's own
    // `.object` is deliberately NOT recursed into (that's exactly the
    // read-only access this classification exists to allow).
    return false;
}

void SmartPointerAnalyzer::classifyReadOnlyFreeFunctionParams(const Program& program) {
    // A parameter that is simply NEVER USED at all (e.g. an empty-bodied
    // `void sink(Node n) {}`) has zero disqualifying uses by construction --
    // but "no evidence it's consumed" is not the same claim as "read-only",
    // and treating it as read-only would be a real regression (confirmed:
    // it silently broke `sink(a)`'s ordinary move semantics in
    // tests/safety/move_then_use.clg, which relies on an unused parameter
    // still consuming its argument, matching every other CeeLing function).
    // Requiring at least one GENUINE read of `paramName.field` (or a falsy/
    // non-null check on the bare parameter) before granting read-only status
    // keeps the classification positive evidence-based, matching CLAUDE.md's
    // "ambiguous -> conservative reject" spirit: no read at all is exactly as
    // ambiguous as a disqualifying use, so it defaults to the ordinary
    // move-by-value behavior instead.
    std::function<bool(const Expression*, const std::string&, const std::string&, const std::set<std::string>&)>
        exprHasGenuineRead = [&](const Expression* expr, const std::string& paramName,
                                  const std::string& currentClass, const std::set<std::string>& okCallees) -> bool {
        if (expr == nullptr) return false;
        if (auto member = dynamic_cast<const MemberAccess*>(expr)) {
            if (auto id = dynamic_cast<const Identifier*>(member->object.get())) {
                if (id->name == paramName) return true;
            }
            return exprHasGenuineRead(member->object.get(), paramName, currentClass, okCallees);
        }
        // `paramName[index]` (yaml-cpp-style operator[] read, e.g.
        // `target["k"]`/`target[i]`) is exactly as genuine a read as
        // `paramName.field` -- confirmed as a real gap via yaml-cling's own
        // Parser::parseBlockMapInto, whose ONLY touch of its `target`
        // parameter is `target[key]` (never a bare `.field` access); without
        // this, that method's parameter never satisfied the genuine-read
        // requirement despite being read-only in every other sense, which in
        // turn meant callers of parseBlockMapInto never saw it as an
        // okCallee, so `parseValueInto`'s OWN target parameter (which passes
        // its argument bare into parseBlockMapInto) was wrongly disqualified
        // too, cascading all the way up to `Parser::parseDocument`'s `root`
        // showing as moved-from.
        if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) {
                if (id->name == paramName) return true;
            }
            return exprHasGenuineRead(sub->object.get(), paramName, currentClass, okCallees) ||
                   exprHasGenuineRead(sub->index.get(), paramName, currentClass, okCallees);
        }
        if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            if (un->operator_str == "!") {
                auto operandId = dynamic_cast<const Identifier*>(un->operand.get());
                if (operandId != nullptr && operandId->name == paramName) return true;
            }
            return exprHasGenuineRead(un->operand.get(), paramName, currentClass, okCallees);
        }
        if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            return exprHasGenuineRead(bin->left.get(), paramName, currentClass, okCallees) ||
                   exprHasGenuineRead(bin->right.get(), paramName, currentClass, okCallees);
        }
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            // A bare paramName passed to an already-read-only-AND-genuinely-
            // read callee parameter (okCallees) counts as a genuine read HERE
            // too, not just a non-disqualifying use -- mirrors
            // paramUseDisqualifiesReadOnly's okCallees exception. Confirmed
            // necessary via a real gap: `Dump(node) { return
            // emitNode(node,...); }` never touches `node.field` directly
            // itself (only emitNode does), so without this, Dump would pass
            // the DISQUALIFICATION check (not disqualified) but fail the
            // GENUINE-READ requirement, staying un-classified forever despite
            // being exactly as read-only as emitNode itself. Method-scoped
            // key tried first, same reasoning as paramUseDisqualifiesReadOnly.
            for (size_t i = 0; i < call->arguments.size(); ++i) {
                auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                if (argId != nullptr && argId->name == paramName && !call->callee) {
                    const std::string methodKey =
                        currentClass + "::" + call->function_name + "::" + std::to_string(i);
                    const std::string freeKey = call->function_name + "::" + std::to_string(i);
                    if ((!currentClass.empty() && okCallees.count(methodKey) > 0) ||
                        okCallees.count(freeKey) > 0) {
                        return true;
                    }
                }
                if (exprHasGenuineRead(call->arguments[i].get(), paramName, currentClass, okCallees)) return true;
            }
            if (call->callee && exprHasGenuineRead(call->callee.get(), paramName, currentClass, okCallees)) {
                return true;
            }
            return false;
        }
        return false;
    };
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyHasGenuineRead = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                  const std::string& currentClass, const std::set<std::string>& okCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                if (exprHasGenuineRead(ret->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (exprHasGenuineRead(decl->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (exprHasGenuineRead(assign->expression.get(), paramName, currentClass, okCallees)) return true;
                if (assign->target &&
                    exprHasGenuineRead(assign->target.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (exprHasGenuineRead(exprStmt->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (exprHasGenuineRead(ifs->condition.get(), paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(ifs->body, paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(ifs->elseBody, paramName, currentClass, okCallees)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (exprHasGenuineRead(whiles->condition.get(), paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(whiles->body, paramName, currentClass, okCallees)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                if (exprHasGenuineRead(fors->container.get(), paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(fors->body, paramName, currentClass, okCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (exprHasGenuineRead(throwStmt->expression.get(), paramName, currentClass, okCallees)) return true;
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyHasGenuineRead(trys->body, paramName, currentClass, okCallees)) return true;
                if (bodyHasGenuineRead(trys->catchBody, paramName, currentClass, okCallees)) return true;
            }
        }
        return false;
    };

    // Every disqualifying-use CHECK (see paramUseDisqualifiesReadOnly) walks a
    // statement body looking for any use of paramName that would require it to
    // be consumed. A statement-level walk (not just returns, unlike
    // inferAliasReturningFreeFunctions) since a disqualifying use can be a
    // plain assignment or bare call argument anywhere in the body, not only in
    // a `return`. Threads `okCallees` through (see paramUseDisqualifiesReadOnly's
    // own comment) so a fixed-point round can re-check with a growing set of
    // already-classified read-only callees.
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyDisqualifies = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                const std::string& currentClass, const std::set<std::string>& okCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(ret->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(decl->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(assign->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (assign->target &&
                    paramUseDisqualifiesReadOnly(assign->target.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(exprStmt->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(ifs->condition.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (bodyDisqualifies(ifs->body, paramName, currentClass, okCallees)) return true;
                if (bodyDisqualifies(ifs->elseBody, paramName, currentClass, okCallees)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(whiles->condition.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (bodyDisqualifies(whiles->body, paramName, currentClass, okCallees)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(fors->container.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
                if (bodyDisqualifies(fors->body, paramName, currentClass, okCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnly(throwStmt->expression.get(), paramName, currentClass, okCallees)) {
                    return true;
                }
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyDisqualifies(trys->body, paramName, currentClass, okCallees)) return true;
                if (bodyDisqualifies(trys->catchBody, paramName, currentClass, okCallees)) return true;
            }
        }
        return false;
    };

    // Fixed-point over ALL free functions AND class methods (mirrors
    // inferAliasReturningMethods's established shape): a function/method
    // passing paramName bare into ANOTHER function/method's already-read-only
    // parameter is itself still read-only (see paramUseDisqualifiesReadOnly's
    // FunctionCall handling) -- but that target might not be classified until
    // a LATER round (e.g. `operator<<` depends on `Dump`, which is a
    // completely ordinary function classified on round 1; `operator<<` itself
    // then only classifies correctly on round 2, once `okCallees` contains
    // "Dump::0"). Bounded by the number of functions+methods (each round must
    // classify at least one new pair to continue, same termination argument
    // as every other fixed point in this file). Methods were added for RB18
    // (yaml_parser.clg's Parser::parseValueInto/parseBlockMapInto/
    // parseBlockSeqInto mutate their `target` parameter in place across a
    // MUTUALLY RECURSIVE set of same-class methods -- exactly the
    // free-function walker shape this mechanism was built for, just one
    // level down inside a class -- confirmed as a real gap via the actual
    // yaml-cling parser, not a hypothetical).
    std::set<std::string> classified;         // "fnName::paramName", free functions
    std::set<std::string> classifiedMethods;  // "ClassName::methodName::paramName"
    bool changed = true;
    while (changed) {
        changed = false;
        // okCallees: "fnName::argIndex" for a read-only free-function param,
        // "ClassName::methodName::argIndex" for a read-only method param --
        // built fresh each round from `classified`/`classifiedMethods` +
        // each function/method's own parameter list, so a newly-classified
        // callee becomes visible starting next round (not the same round it
        // was itself classified in, keeping the iteration simple and
        // monotonic).
        std::set<std::string> okCallees;
        for (const auto& stmt : program.statements) {
            if (auto fn2 = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (size_t i = 0; i < fn2->parameters.size(); ++i) {
                    if (classified.count(fn2->name + "::" + fn2->parameters[i].second) > 0) {
                        okCallees.insert(fn2->name + "::" + std::to_string(i));
                    }
                }
            } else if (auto cls2 = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls2->members) {
                    auto method2 = dynamic_cast<const MethodDeclaration*>(member.get());
                    if (method2 == nullptr) continue;
                    for (size_t i = 0; i < method2->parameters.size(); ++i) {
                        const std::string key =
                            cls2->name + "::" + method2->name + "::" + method2->parameters[i].second;
                        if (classifiedMethods.count(key) > 0) {
                            okCallees.insert(cls2->name + "::" + method2->name + "::" + std::to_string(i));
                        }
                    }
                }
            }
        }

        for (const auto& stmt : program.statements) {
            if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (const auto& [ptype, pname] : fn->parameters) {
                    const std::string classifiedKey = fn->name + "::" + pname;
                    if (classified.count(classifiedKey) > 0) continue;  // already decided

                    // Only object-typed parameters are ever a move-vs-share question --
                    // a primitive/string/container parameter is never smart-pointer-
                    // backed in the first place, so read-only-ness is moot for it.
                    bool isClassType = false;
                    for (const auto& s2 : program.statements) {
                        if (auto cls = dynamic_cast<const ClassDeclaration*>(s2.get())) {
                            if (cls->name == ptype) { isClassType = true; break; }
                        }
                    }
                    if (!isClassType) continue;
                    if (!bodyDisqualifies(fn->body, pname, "", okCallees) &&
                        bodyHasGenuineRead(fn->body, pname, "", okCallees)) {
                        classified.insert(classifiedKey);
                        changed = true;
                    }
                }
            } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls->members) {
                    auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                    // A specialization is excluded -- classifiedKey below is
                    // keyed by BARE method name (no specializationKey), so a
                    // specialization's own param-read pattern could
                    // classify (or fail to classify) the SAME key as its
                    // primary based on unrelated body content. The
                    // specialization's out-parameter is already correctly
                    // handled by its own dedicated mechanism (DanglingChecker/
                    // AstEmitter's outParamIndex_), which this pass has no
                    // role in.
                    if (method == nullptr || !method->specializationArg.empty()) continue;
                    for (const auto& [ptype, pname] : method->parameters) {
                        const std::string classifiedKey = cls->name + "::" + method->name + "::" + pname;
                        if (classifiedMethods.count(classifiedKey) > 0) continue;  // already decided

                        bool isClassType = false;
                        for (const auto& s2 : program.statements) {
                            if (auto cls2 = dynamic_cast<const ClassDeclaration*>(s2.get())) {
                                if (cls2->name == ptype) { isClassType = true; break; }
                            }
                        }
                        if (!isClassType) continue;
                        if (!bodyDisqualifies(method->body, pname, cls->name, okCallees) &&
                            bodyHasGenuineRead(method->body, pname, cls->name, okCallees)) {
                            classifiedMethods.insert(classifiedKey);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    for (const auto& key : classified) {
        readOnlyFreeFunctionParams_.insert(key);
    }
    for (const auto& key : classifiedMethods) {
        readOnlyMethodParams_.insert(key);
    }
    // Dual-register under the qualified name too -- see collectCalleeParams's
    // matching comment; isReadOnlyFreeFunctionParam is queried with whatever
    // spelling the CALL SITE used (call->function_name), which is qualified
    // from outside the namespace.
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr || fn->namespaceName.empty()) continue;
        for (const auto& [ptype, pname] : fn->parameters) {
            if (classified.count(fn->name + "::" + pname) > 0) {
                readOnlyFreeFunctionParams_.insert(fn->namespaceName + "::" + fn->name + "::" + pname);
            }
        }
    }
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr || cls->namespaceName.empty()) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr || !method->specializationArg.empty()) continue;
            for (const auto& [ptype, pname] : method->parameters) {
                if (classifiedMethods.count(cls->name + "::" + method->name + "::" + pname) > 0) {
                    readOnlyMethodParams_.insert(cls->namespaceName + "::" + cls->name + "::" + method->name +
                                                  "::" + pname);
                }
            }
        }
    }
}

bool SmartPointerAnalyzer::isReadOnlyFreeFunctionParam(const std::string& fnName,
                                                        const std::string& paramName) const {
    return readOnlyFreeFunctionParams_.count(fnName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyMethodParam(const std::string& className, const std::string& methodName,
                                                  const std::string& paramName) const {
    return readOnlyMethodParams_.count(className + "::" + methodName + "::" + paramName) > 0;
}

void SmartPointerAnalyzer::firstPassCollectReferences(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto classDecl = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames.insert(classDecl->name);
            classRelationships[classDecl->name].className = classDecl->name;
        }
    }

    // Classes first, in base-before-derived order, so analyzeClassDeclaration can
    // re-register an inherited field under the derived scope from the base's
    // already-collected members. Everything else (functions, main) is analyzed
    // afterward in ordinary input order -- it can only reference classes, never be
    // referenced by one, so its relative position to the class walk doesn't matter.
    for (const ClassDeclaration* cls : classesInBaseFirstOrder(program)) {
        analyzeStatement(*cls);
    }

    for (const auto& stmt : program.statements) {
        if (dynamic_cast<const ClassDeclaration*>(stmt.get()) != nullptr) continue;
        analyzeStatement(*stmt);
    }
}

void SmartPointerAnalyzer::analyzeStatement(const Statement& stmt, const std::string& currentClass) {
    if (auto classDecl = dynamic_cast<const ClassDeclaration*>(&stmt)) {
        analyzeClassDeclaration(*classDecl);
    } else if (auto varDecl = dynamic_cast<const VariableDeclaration*>(&stmt)) {
        analyzeVariableDeclaration(*varDecl, currentClass);
    } else if (auto assignment = dynamic_cast<const AssignmentStatement*>(&stmt)) {
        analyzeAssignmentStatement(*assignment, currentClass);
    } else if (auto funcDecl = dynamic_cast<const FunctionDeclaration*>(&stmt)) {
        registerParameters(funcDecl->parameters, funcDecl->name);
        for (const auto& bodyStmt : funcDecl->body) {
            analyzeStatement(*bodyStmt, funcDecl->name);
        }
    } else if (auto mainFunc = dynamic_cast<const MainFunction*>(&stmt)) {
        for (const auto& bodyStmt : mainFunc->body) {
            analyzeStatement(*bodyStmt, "main");
        }
    } else if (auto ctorDecl = dynamic_cast<const ConstructorDeclaration*>(&stmt)) {
        const std::string ctorScope = currentClass.empty() ? ctorDecl->name : currentClass + "::" + ctorDecl->name;
        registerParameters(ctorDecl->parameters, ctorScope);
        for (const auto& bodyStmt : ctorDecl->body) {
            analyzeStatement(*bodyStmt, ctorScope);
        }
    } else if (auto methodDecl = dynamic_cast<const MethodDeclaration*>(&stmt)) {
        const std::string methodScope = currentClass.empty() ? methodDecl->name : currentClass + "::" + methodDecl->name;
        registerParameters(methodDecl->parameters, methodScope);
        for (const auto& bodyStmt : methodDecl->body) {
            analyzeStatement(*bodyStmt, methodScope);
        }
    }
}

void SmartPointerAnalyzer::registerParameters(
    const std::vector<std::pair<std::string, std::string>>& params, const std::string& scopeName) {
    // Give each object-typed parameter a bare variableReferences entry so
    // getPointerType(className, paramName) has something to find instead of always
    // missing and falling back to the SHARED default (see AstEmitter::emitFunction).
    // Deliberately minimal: only registerReference + registerSymbolType, matching
    // just the identity (scope, class, name) a fresh local gets on first sight.
    // NOT setting isAssignedFrom/assignedFromVars or any bidirectional/back-reference
    // bookkeeping here -- those heuristics are tuned around locals and fields observed
    // via full statement analysis, and wiring a parameter into them was not verified
    // safe. A parameter that is never itself assigned into a field defaults to
    // PointerType::UNIQUE from secondPassDeterminePointerTypes' base case, same as a
    // fresh local would; a parameter that IS stored into a field (e.g. `next = child;`)
    // still gets promoted to SHARED via scanSource's independent regex pass, which
    // already covers that case today.
    for (const auto& [ptype, pname] : params) {
        if (classNames.find(ptype) == classNames.end()) continue;
        registerReference(scopeName, ptype, pname, 0);
        registerSymbolType(scopeName, pname, ptype);
    }
}

void SmartPointerAnalyzer::analyzeVariableDeclaration(const VariableDeclaration& decl, const std::string& currentClass) {
    if (classNames.find(decl.type) == classNames.end()) {
        return;
    }

    registerReference(currentClass, decl.type, decl.identifier, 0);
    registerSymbolType(currentClass.empty() ? kGlobalScope : currentClass, decl.identifier, decl.type);

    if (!decl.expression) {
        return;
    }

    if (auto identifier = dynamic_cast<const Identifier*>(decl.expression.get())) {
        const std::string key = buildKey(currentClass, decl.type, decl.identifier);
        auto& ref = variableReferences[key];
        ref.isAssignedFrom = true;
        ref.assignedFromVars.insert(identifier->name);
    }
}

void SmartPointerAnalyzer::analyzeAssignmentStatement(const AssignmentStatement& stmt, const std::string& currentClass) {
    const ParsedValue rhs = parseValue(stmt.expression ? stmt.expression->toString() : "");
    if (!rhs.isIdentifier && !rhs.isMemberAccess) {
        return;
    }

    const std::string scopeName = normalizeScope(currentClass);
    for (auto& [key, ref] : variableReferences) {
        if (ref.variableName == stmt.variable && ref.containingScope == scopeName) {
            ref.isAssignedFrom = true;
            ref.assignedFromVars.insert(rhs.isIdentifier ? rhs.identifier : rhs.memberName);
        }
    }
}

void SmartPointerAnalyzer::analyzeClassDeclaration(const ClassDeclaration& cls) {
    classNames.insert(cls.name);
    classRelationships[cls.name].className = cls.name;
    // Also register the QUALIFIED name (`YAML::Node`): analyzeVariableDeclaration/
    // registerParameters gate their own registration on `classNames.find(type) !=
    // end()`, keyed by the literal declared-type STRING (which for a `YAML::Node a;`
    // local is "YAML::Node", not "Node"). Without this, such a local is silently
    // never registered as a VariableReference at all -- not "wrongly defaults to
    // shared", the TRUE getPointerType fallback (confirmed by probe: a plain,
    // never-shared class correctly gets UNIQUE via the ordinary two-pass decision,
    // because it IS registered; a namespaced one wasn't, and got the raw fallback
    // instead, which happens to also be SHARED -- easy to mistake for "no bug" until
    // move-tracking is checked specifically). classRelationships stays keyed by the
    // SIMPLE name only (cls.name) -- back-reference/bidirectional detection doesn't
    // need a second entry, only the classNames gate does.
    if (!cls.namespaceName.empty()) {
        classNames.insert(cls.namespaceName + "::" + cls.name);
    }

    // Inherited fields need their own entries under the DERIVED class's scope, not
    // just the base's: this analyzer keys everything by (scope, class, variable), so
    // a derived instance's inherited field would otherwise have no entry at all and
    // getPointerType would fall through to its SHARED default -- silently making
    // every inherited field a shared_ptr regardless of actual ownership shape.
    // Unlike the other three modules' flat maps, "flattening" here means re-running
    // the base's member registration in the derived scope. Base-before-derived
    // ordering is guaranteed by firstPassCollectReferences's caller.
    if (!cls.baseClass.empty()) {
        if (auto baseIt = classMembersByClass_.find(cls.baseClass);
            baseIt != classMembersByClass_.end()) {
            for (const auto& [fieldName, fieldType] : baseIt->second) {
                registerReference(cls.name, fieldType, fieldName, 0);
                registerSymbolType(cls.name, fieldName, fieldType);
                classRelationships[cls.name].memberVariables.insert(fieldName);
                classRelationships[cls.name].references.insert(fieldType);
                classRelationships[fieldType].referencedBy.insert(cls.name);
                classMembersByClass_[cls.name][fieldName] = fieldType;
            }
        }
    }

    for (const auto& member : cls.members) {
        if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
            if (classNames.find(field->type) != classNames.end()) {
                classMembersByClass_[cls.name][field->identifier] = field->type;
            }
        }
        analyzeStatement(*member, cls.name);
    }
}

void SmartPointerAnalyzer::scanSource(const std::string& source) {
    const std::regex classRegex(R"(^\s*class\s+([A-Za-z_][A-Za-z0-9_]*))");
    const std::regex memberDeclRegex(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
    const std::regex variableDeclRegex(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+);\s*$)");
    const std::regex assignmentRegex(R"(^\s*([A-Za-z_][A-Za-z0-9_]*(?:\s*(?:\.|->)\s*[A-Za-z_][A-Za-z0-9_]*)*)\s*=\s*(.+);\s*$)");
    const std::regex functionRegex(R"(^\s*(?:[A-Za-z_][A-Za-z0-9_]*\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*\)\s*(?:const)?\s*\{?\s*$)");

    std::istringstream lines(source);
    std::string line;
    std::string currentClass;
    std::string currentFunction;
    int braceDepth = 0;
    int classBraceDepth = -1;
    int functionBraceDepth = -1;
    int lineNumber = 0;

    while (std::getline(lines, line)) {
        ++lineNumber;
        const std::string trimmedLine = trim(line);
        std::smatch match;

        parseAnnotationLine(trimmedLine, lineNumber);

        if (std::regex_search(trimmedLine, match, classRegex)) {
            currentClass = match[1].str();
            classBraceDepth = braceDepth;
            classNames.insert(currentClass);
            classRelationships[currentClass].className = currentClass;
        } else if (trimmedLine.find('(') != std::string::npos &&
                   trimmedLine.find(')') != std::string::npos &&
                   trimmedLine.find(';') == std::string::npos &&
                   trimmedLine.rfind("if", 0) != 0 &&
                   trimmedLine.rfind("while", 0) != 0 &&
                   trimmedLine.rfind("for", 0) != 0 &&
                   trimmedLine.rfind("switch", 0) != 0 &&
                   std::regex_match(trimmedLine, match, functionRegex)) {
            currentFunction = match[1].str();
            functionBraceDepth = braceDepth;
        }

        const std::string scopeName = chooseMethodLikeScope(currentClass, currentFunction);

        if (!currentClass.empty() && trimmedLine.find('(') == std::string::npos &&
            std::regex_match(trimmedLine, match, memberDeclRegex)) {
            const std::string memberType = match[1].str();
            const std::string memberName = match[2].str();
            if (classNames.find(memberType) != classNames.end()) {
                registerReference(currentClass, memberType, memberName, lineNumber);
                classRelationships[currentClass].memberVariables.insert(memberName);
                classRelationships[currentClass].references.insert(memberType);
                classRelationships[memberType].referencedBy.insert(currentClass);
            }
        }

        bool handledDeclaration = false;
        if (std::regex_match(trimmedLine, match, variableDeclRegex)) {
            const std::string declaredType = match[1].str();
            const std::string declaredName = match[2].str();
            const std::string rhsText = match[3].str();

            if (classNames.find(declaredType) != classNames.end()) {
                registerReference(scopeName, declaredType, declaredName, lineNumber);
                registerSymbolType(scopeName, declaredName, declaredType);

                const ParsedValue rhs = parseValue(rhsText);
                const std::string key = buildKey(scopeName, declaredType, declaredName);
                auto& ref = variableReferences[key];

                if (rhs.isIdentifier) {
                    ref.isAssignedFrom = true;
                    ref.assignedFromVars.insert(rhs.identifier);
                    for (auto& [otherKey, otherRef] : variableReferences) {
                        if (otherRef.variableName == rhs.identifier) {
                            otherRef.isAssignedTo = true;
                            otherRef.assignedToVars.insert(declaredName);
                        }
                    }
                } else if (rhs.isMemberAccess) {
                    ref.isAssignedFrom = true;
                    ref.assignedFromVars.insert(rhs.memberName);
                    for (auto& [otherKey, otherRef] : variableReferences) {
                        if (otherRef.variableName == rhs.memberName) {
                            otherRef.isAssignedTo = true;
                            otherRef.assignedToVars.insert(declaredName);
                        }
                    }
                }
            }

            handledDeclaration = true;
        }

        if (!handledDeclaration && std::regex_match(trimmedLine, match, assignmentRegex)) {
            const std::string lhsText = trim(match[1].str());
            const std::string rhsText = trim(match[2].str());
            const ParsedValue lhs = parseValue(lhsText);
            const ParsedValue rhs = parseValue(rhsText);

            if (lhs.isMemberAccess) {
                for (auto& [key, ref] : variableReferences) {
                    if (ref.variableName == lhs.memberName) {
                        ref.isAssignedFrom = ref.isAssignedFrom || rhs.isIdentifier || rhs.isMemberAccess;
                        if (rhs.isIdentifier) {
                            ref.assignedFromVars.insert(rhs.identifier);
                        } else if (rhs.isMemberAccess) {
                            ref.assignedFromVars.insert(rhs.memberName);
                        }
                    }
                }

                if (rhs.isIdentifier) {
                    recordBidirectionalAssignment(lhs.objectName, lhs.memberName, rhs.identifier, lineNumber, scopeName);
                }
            } else if (lhs.isIdentifier && (rhs.isIdentifier || rhs.isMemberAccess)) {
                for (auto& [key, ref] : variableReferences) {
                    if (ref.variableName == lhs.identifier &&
                        (ref.containingScope == normalizeScope(scopeName) ||
                         (!currentClass.empty() && ref.containingScope == currentClass))) {
                        ref.isAssignedFrom = true;
                        ref.assignedFromVars.insert(rhs.isIdentifier ? rhs.identifier : rhs.memberName);
                    }
                }
            }

            if (lhs.isIdentifier) {
                for (auto& [key, ref] : variableReferences) {
                    if (ref.variableName == lhs.identifier &&
                        (ref.containingScope == normalizeScope(scopeName) ||
                         (!currentClass.empty() && ref.containingScope == currentClass))) {
                        if (rhs.isIdentifier) {
                            ref.assignedFromVars.insert(rhs.identifier);
                        } else if (rhs.isMemberAccess) {
                            ref.assignedFromVars.insert(rhs.memberName);
                        }
                    }
                }
            }

            if (rhs.isIdentifier) {
                for (auto& [key, ref] : variableReferences) {
                    if (ref.variableName == rhs.identifier) {
                        ref.isAssignedTo = true;
                        ref.assignedToVars.insert(lhs.isIdentifier ? lhs.identifier : lhs.memberName);
                    }
                }
            } else if (rhs.isMemberAccess) {
                for (auto& [key, ref] : variableReferences) {
                    if (ref.variableName == rhs.memberName) {
                        ref.isAssignedTo = true;
                        ref.assignedToVars.insert(lhs.isIdentifier ? lhs.identifier : lhs.memberName);
                    }
                }
            }
        }

        braceDepth += countChar(line, '{');
        braceDepth -= countChar(line, '}');

        if (!currentFunction.empty() && functionBraceDepth >= 0 && braceDepth <= functionBraceDepth) {
            currentFunction.clear();
            functionBraceDepth = -1;
        }
        if (!currentClass.empty() && classBraceDepth >= 0 && braceDepth <= classBraceDepth) {
            currentClass.clear();
            classBraceDepth = -1;
        }
    }

    for (auto& [key, ref] : variableReferences) {
        if (ref.containingScope != ref.className) {
            continue;
        }

        const std::regex memberAliasPattern(
            "\\b" + ref.variableName +
            R"(\s*=\s*(?!new\b)(?!nullptr\b)([A-Za-z_][A-Za-z0-9_]*(?:\s*(?:\.|->)\s*[A-Za-z_][A-Za-z0-9_]*)?)\s*;)");
        std::smatch match;
        if (std::regex_search(source, match, memberAliasPattern)) {
            ref.isAssignedFrom = true;
            ref.assignedFromVars.insert(match[1].str());
        }
    }

    applyExplicitHints();
}

void SmartPointerAnalyzer::parseAnnotationLine(const std::string& trimmedLine, int lineNumber) {
    std::smatch match;
    if (!std::regex_match(trimmedLine, match,
                          std::regex(R"(^//\s*@ptr\s+(unique|shared|weak)\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?)\s*$)"))) {
        return;
    }

    const PointerType type = parsePointerType(match[1].str());
    const std::string target = match[2].str();

    auto it = explicitHints.find(target);
    if (it != explicitHints.end() && it->second != type) {
        addDiagnostic(lineNumber,
                      "Conflicting pointer annotation for '" + target + "'.",
                      "Keep a single @ptr hint per target.");
    }

    explicitHints[target] = type;
    explicitHintLines[target] = lineNumber;
}

void SmartPointerAnalyzer::registerReference(const std::string& scopeName, const std::string& className,
                                             const std::string& variableName, int lineNumber) {
    const std::string key = buildKey(scopeName, className, variableName);
    auto& ref = variableReferences[key];
    ref.className = className;
    ref.variableName = variableName;
    ref.containingScope = normalizeScope(scopeName);
    if (lineNumber != 0) {
        ref.line = lineNumber;
    }
}

void SmartPointerAnalyzer::registerSymbolType(const std::string& scopeName, const std::string& symbolName,
                                              const std::string& className) {
    symbolTypesByScope[makeSymbolKey(normalizeScope(scopeName), symbolName)] = className;
    if (scopeName != kGlobalScope) {
        symbolTypesByScope[makeSymbolKey(kGlobalScope, symbolName)] = className;
    }
}

void SmartPointerAnalyzer::recordBidirectionalAssignment(const std::string& lhsObject, const std::string& lhsMember,
                                                         const std::string& rhsObject, int lineNumber,
                                                         const std::string& scopeName) {
    const std::string scopedKey = normalizeScope(scopeName) + "|" + lhsObject + "|" + rhsObject;
    const std::string reverseKey = normalizeScope(scopeName) + "|" + rhsObject + "|" + lhsObject;

    const auto currentTypeIt = symbolTypesByScope.find(makeSymbolKey(normalizeScope(scopeName), lhsObject));
    const auto reverseTypeIt = symbolTypesByScope.find(makeSymbolKey(normalizeScope(scopeName), rhsObject));
    if (currentTypeIt == symbolTypesByScope.end() || reverseTypeIt == symbolTypesByScope.end()) {
        return;
    }

    const std::string ownerMemberKey = makeMemberKey(currentTypeIt->second, lhsMember);

    auto reverseIt = bidirectionalAssignmentSeeds.find(reverseKey);
    if (reverseIt == bidirectionalAssignmentSeeds.end()) {
        bidirectionalAssignmentSeeds[scopedKey] = {ownerMemberKey, lineNumber};
        return;
    }

    const std::string reverseMemberKey = reverseIt->second.first;
    const int reverseLine = reverseIt->second.second;
    const std::string reverseMemberName = reverseMemberKey.substr(reverseMemberKey.find('.') + 1);

    std::string backMemberKey = ownerMemberKey;
    std::string forwardMemberKey = reverseMemberKey;

    if (isBackReferenceName(reverseMemberName) && !isBackReferenceName(lhsMember)) {
        backMemberKey = reverseMemberKey;
        forwardMemberKey = ownerMemberKey;
    } else if (!isBackReferenceName(reverseMemberName) && isBackReferenceName(lhsMember)) {
        backMemberKey = ownerMemberKey;
        forwardMemberKey = reverseMemberKey;
    } else if (!isBackReferenceName(reverseMemberName) && !isBackReferenceName(lhsMember)) {
        addDiagnostic(lineNumber,
                      "Bidirectional assignment detected between '" + lhsObject + "." + lhsMember +
                          "' and '" + rhsObject + "." + reverseMemberName + "'.",
                      "Annotate the backward edge with //@ptr weak if you want a different choice.");
    }

    bidirectionalBackMembers.insert(backMemberKey);
    bidirectionalOwnerMembers.insert(forwardMemberKey);
    classesNeedingSharedRoots.insert(currentTypeIt->second);
    classesNeedingSharedRoots.insert(reverseTypeIt->second);
    classRelationships[currentTypeIt->second].hasBidirectionalAssignment = true;
    classRelationships[reverseTypeIt->second].hasBidirectionalAssignment = true;

    for (auto& [key, ref] : variableReferences) {
        const std::string memberKey = makeMemberKey(ref.className, ref.variableName);
        if (memberKey == backMemberKey) {
            ref.isBackReference = true;
            ref.participatesInBidirectionalAssignment = true;
        } else if (memberKey == forwardMemberKey) {
            ref.participatesInBidirectionalAssignment = true;
        }
    }

    std::cerr << "Detected bidirectional assignment between lines "
              << reverseLine << " and " << lineNumber << std::endl;
}

void SmartPointerAnalyzer::secondPassDeterminePointerTypes() {
    detectBackReferences();
    applyExplicitHints();
    reportLines.clear();

    for (auto& [key, ref] : variableReferences) {
        PointerType pointerType = PointerType::UNIQUE;
        std::string reason = "default unique ownership";

        const std::string exactMemberKey = makeMemberKey(ref.className, ref.variableName);
        const auto explicitIt = explicitHints.find(exactMemberKey);
        const auto fallbackHintIt = explicitHints.find(ref.variableName);

        if (explicitIt != explicitHints.end()) {
            pointerType = explicitIt->second;
            ref.hasExplicitHint = true;
            reason = "explicit @ptr annotation";
        } else if (fallbackHintIt != explicitHints.end()) {
            pointerType = fallbackHintIt->second;
            ref.hasExplicitHint = true;
            reason = "explicit @ptr annotation";
        } else if (ref.isBackReference || bidirectionalBackMembers.find(exactMemberKey) != bidirectionalBackMembers.end()) {
            pointerType = PointerType::WEAK;
            reason = ref.participatesInBidirectionalAssignment
                ? "bidirectional assignment closed a cycle"
                : "back-reference heuristic matched a backward edge";
            classesNeedingSharedRoots.insert(ref.className);
            classRelationships[ref.className].hasBackReference = true;
        } else if (ref.participatesInBidirectionalAssignment ||
                   bidirectionalOwnerMembers.find(exactMemberKey) != bidirectionalOwnerMembers.end()) {
            pointerType = PointerType::SHARED;
            reason = "bidirectional ownership needs a shared forward edge";
            classesNeedingSharedRoots.insert(ref.className);
        } else if (ref.containingScope == ref.className && ref.isAssignedFrom &&
                   !ref.assignedFromVars.empty()) {
            pointerType = PointerType::SHARED;
            reason = "member receives an existing object reference, so ownership is shared";
        } else if (classesNeedingSharedRoots.find(ref.className) != classesNeedingSharedRoots.end() &&
                   ref.containingScope != ref.className) {
            pointerType = PointerType::SHARED;
            reason = "instances of this class participate in weak back-references";
        } else if (isSharedOwnership(ref.containingScope, ref.className, ref.variableName)) {
            pointerType = PointerType::SHARED;
            reason = "data-flow analysis found multiple live aliases";
        }

        pointerDecisions[key] = pointerType;
        pointerReasons[key] = reason;
        ref.reason = reason;

        std::ostringstream report;
        report << ref.className << "." << ref.variableName
               << " -> " << pointerTypeName(pointerType)
               << " because " << reason;
        if (ref.line > 0) {
            report << " (line " << ref.line << ")";
        }
        reportLines.push_back(report.str());
    }

    std::sort(reportLines.begin(), reportLines.end());
}

void SmartPointerAnalyzer::detectBackReferences() {
    std::map<std::string, std::vector<std::string>> classMembersByType;

    for (const auto& [key, ref] : variableReferences) {
        if (ref.containingScope == ref.className) {
            classMembersByType[ref.containingScope + ":" + ref.className].push_back(ref.variableName);
        }
    }

    for (auto& [key, ref] : variableReferences) {
        const std::string groupKey = ref.containingScope + ":" + ref.className;
        const auto groupIt = classMembersByType.find(groupKey);
        if (groupIt != classMembersByType.end() && groupIt->second.size() >= 2 &&
            isBackReferenceName(ref.variableName)) {
            ref.isBackReference = true;
            classRelationships[ref.className].hasBackReference = true;
        }
    }
}

bool SmartPointerAnalyzer::isSharedOwnership(const std::string& containingScope, const std::string& className,
                                             const std::string& variableName) const {
    const std::string key = buildKey(containingScope, className, variableName);
    const auto it = variableReferences.find(key);
    if (it == variableReferences.end()) {
        return false;
    }

    const auto& ref = it->second;
    return ref.assignedToVars.size() > 1 || ref.assignedFromVars.size() > 1 ||
           (ref.isAssignedFrom && ref.isAssignedTo);
}

void SmartPointerAnalyzer::applyExplicitHints() {
    for (const auto& [target, line] : explicitHintLines) {
        bool matched = false;
        for (auto& [key, ref] : variableReferences) {
            const std::string memberKey = makeMemberKey(ref.className, ref.variableName);
            if (target == ref.variableName || target == memberKey) {
                ref.hasExplicitHint = true;
                matched = true;
            }
        }

        if (!matched) {
            addDiagnostic(line,
                          "Pointer hint for '" + target + "' did not match any discovered class pointer.",
                          "Use //@ptr <type> Class.member or //@ptr <type> member.");
        }
    }
}

void SmartPointerAnalyzer::addDiagnostic(int line, const std::string& message, const std::string& suggestion) {
    diagnostics.push_back({line, message, suggestion});
}

std::string SmartPointerAnalyzer::buildKey(const std::string& containingScope, const std::string& className,
                                           const std::string& variableName) const {
    return normalizeScope(containingScope) + "|" + className + "." + variableName;
}

std::string SmartPointerAnalyzer::normalizeScope(const std::string& containingScope) const {
    return containingScope.empty() ? kGlobalScope : containingScope;
}

} // namespace cling
