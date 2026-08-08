#include "smart_pointer_analyzer.h"

#include "class_hierarchy.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
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

    firstPassCollectReferences(program);
    if (!source.empty()) {
        scanSource(source);
    }
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

void SmartPointerAnalyzer::scanContainerElementKinds(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            for (const auto& inner : fn->body) scanContainerElementKindsInStatement(*inner);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            for (const auto& inner : mn->body) scanContainerElementKindsInStatement(*inner);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    for (const auto& inner : method->body) scanContainerElementKindsInStatement(*inner);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    for (const auto& inner : ctor->body) scanContainerElementKindsInStatement(*inner);
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

void SmartPointerAnalyzer::scanContainerElementKindsInStatement(const Statement& stmt) {
    // Only need to find `<container>.push_back(<arg>)` call expressions, wherever
    // they appear (as a bare expression statement or nested in an initializer) --
    // recurse into every statement shape that can contain one.
    auto visitExpr = [this](const Expression* expr) {
        auto call = dynamic_cast<const FunctionCall*>(expr);
        if (call == nullptr || !call->callee) return;
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr || member->member != "push_back" || call->arguments.empty()) return;
        const Identifier* recv = asIdentifier(member->object.get());
        const Identifier* arg = asIdentifier(call->arguments[0].get());
        if (recv == nullptr || arg == nullptr) return;

        // Resolve the argument's OWN already-decided pointer kind via its symbol
        // type (registered the same way locals/parameters/fields already are).
        const auto typeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, arg->name));
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
        for (const auto& inner : ifs->body) scanContainerElementKindsInStatement(*inner);
        for (const auto& inner : ifs->elseBody) scanContainerElementKindsInStatement(*inner);
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(&stmt)) {
        for (const auto& inner : whiles->body) scanContainerElementKindsInStatement(*inner);
    } else if (auto fors = dynamic_cast<const ForStatement*>(&stmt)) {
        for (const auto& inner : fors->body) scanContainerElementKindsInStatement(*inner);
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
        if (getPointerType(fieldTypeIt->second, fieldName) != PointerType::SHARED) continue;

        // The parameter's own type tells us which (class, name) key to promote --
        // registerParameters already gave every object-typed parameter a bare
        // variableReferences entry to promote here.
        auto paramTypeIt = std::find_if(params.begin(), params.end(),
            [&](const auto& p) { return p.second == rhsId->name; });
        if (paramTypeIt == params.end()) continue;
        if (classNames.find(paramTypeIt->first) == classNames.end()) continue;

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
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
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

    // Resolve a call/constructor expression's target parameter list, then promote
    // every argument bound to an already-SHARED parameter to SHARED itself --
    // otherwise the caller's own local stays UNIQUE and the promotion sub-pass 1
    // just did is invisible at the call site (AstEmitter would still emit
    // unique_ptr for the caller's variable). Argument resolution is deliberately
    // lightweight (bare identifier only, via symbolTypesByScope's global-scope
    // fallback), matching this codebase's existing "not full type inference"
    // conservatism elsewhere (e.g. DanglingChecker::asObjectLocalName).
    auto promoteArgsAgainstParams = [this](
        const std::vector<ExpressionPtr>& args,
        const std::vector<std::pair<std::string, std::string>>& params) {
        for (size_t i = 0; i < args.size() && i < params.size(); ++i) {
            const auto& [paramType, paramName] = params[i];
            if (classNames.find(paramType) == classNames.end()) continue;
            if (getPointerType(paramType, paramName) != PointerType::SHARED) continue;

            auto argId = dynamic_cast<const Identifier*>(args[i].get());
            if (argId == nullptr) continue;
            const auto argTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, argId->name));
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
                        const auto recvTypeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, recvId->name));
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
            for (const auto& inner : fn->body) visitStmt(inner.get());
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            for (const auto& inner : mn->body) visitStmt(inner.get());
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    for (const auto& inner : method->body) visitStmt(inner.get());
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    for (const auto& inner : ctor->body) visitStmt(inner.get());
                }
            }
        }
    }
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
