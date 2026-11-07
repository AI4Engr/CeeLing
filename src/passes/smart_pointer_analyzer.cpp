#include "passes/smart_pointer_analyzer.h"
#include "syntax/ast_walk.h"

#include "syntax/class_hierarchy.h"
#include "facts/library_summary.h"

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

void SmartPointerAnalyzer::setDemands(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        std::istringstream in(line);
        std::string kind, what, name;
        if (!(in >> kind >> what >> name) || kind != "shared") continue;
        if (what == "field") demandedFields_.insert(name);
        else if (what == "elements") demandedElements_.insert(name);
        else if (what == "return") demandedReturns_.insert(name);
    }
}

std::vector<std::string> SmartPointerAnalyzer::exportDemands(const Program& program) const {
    std::set<std::string> out;                      // (a set: sorted, no repeats, so the file is the same every time)
    // What this file's own code reaches in other files' classes: `x.member` (a field or container field) and `x.method(...)`. The analysis also promotes by
    // name alone, which says nothing about a member this file never touches, so only reached members are demanded.
    std::set<std::string> reached;
    for (const auto& stmt : program.statements) {
        if (stmt->isExternalDeclaration) continue;
        astwalk::walkStatement(const_cast<Statement*>(stmt.get()), [&](ASTNode& node) {
            if (auto member = dynamic_cast<MemberAccess*>(&node)) reached.insert(member->member);
        });
    }
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr || !cls->isExternalDeclaration) continue;       // only the classes of OTHER files: this file's own are its own business
        for (const auto& member : cls->members) {
            if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                if (reached.count(field->identifier) == 0) continue;
                if (classNames.find(field->type) != classNames.end()) {
                    const std::string key = buildKey(cls->name, field->type, field->identifier);
                    auto it = pointerDecisions.find(key);
                    if (it != pointerDecisions.end() && it->second == PointerType::SHARED) {
                        auto why = pointerReasons.find(key);
                        out.insert("shared field " + cls->name + "." + field->identifier + "  # " + (why != pointerReasons.end() ? why->second : std::string("no reason recorded")));
                    }
                } else if (isVectorType(field->type) && classNames.count(vectorElementType(field->type)) > 0 && containerElementIsShared(field->identifier)) {
                    out.insert("shared elements " + field->identifier);
                }
            } else if (auto container = dynamic_cast<const ContainerDeclaration*>(member.get())) {
                if (reached.count(container->identifier) == 0) continue;
                if (isVectorType(container->type) && classNames.count(vectorElementType(container->type)) > 0 && containerElementIsShared(container->identifier)) {
                    out.insert("shared elements " + container->identifier);
                }
            } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                if (reached.count(method->name) == 0) continue;
                if (method->specializationArg.empty() && classNames.count(method->returnType) > 0 &&
                    getMethodReturnPointerType(cls->name, method->name) == PointerType::SHARED) {
                    out.insert("shared return " + cls->name + "::" + method->name);
                }
            }
        }
    }
    return std::vector<std::string>(out.begin(), out.end());
}

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

    // what the files that include a class of this file need from it (setDemands); same plumbing as a written //@ptr hint
    for (const std::string& field : demandedFields_) explicitHints[field] = PointerType::SHARED;

    containerElementShared_.clear();
    classMembersByClass_.clear();
    methodReturnKinds_.clear();
    for (const std::string& method : demandedReturns_) methodReturnKinds_[method] = PointerType::SHARED;
    classesNeedingSharedFromThis_.clear();
    valueTypeClasses_.clear();
    readOnlyFreeFunctionParams_.clear();
    readOnlyMethodParams_.clear();
    readOnlyContainerParams_.clear();
    readOnlyMethodContainerParams_.clear();
    heapPureFreeFunctions_.clear();
    heapPureMethods_.clear();
    lastUseFieldAssignedParams_.clear();
    lastUseMethodFieldAssignedParams_.clear();

    // Each classification reads only the raw Program and writes its own tables, so the order among them does not matter.
    classifyValueTypeClasses(program);
    classifyReadOnlyFreeFunctionParams(program);
    classifyReadOnlyContainerParams(program);
    classifyReadOnlyStringParams(program);
    classifyHeapPureFunctions(program);
    classifyLastUseFieldAssignedParams(program);

    firstPassCollectReferences(program);
    if (!source.empty()) {
        scanSource(source);
    }
    // Runs before secondPassDeterminePointerTypes: a field decided shared here is written into explicitHints and picked up through the same lookup a //@ptr hint uses.
    inferAliasReturningMethods(program);
    secondPassDeterminePointerTypes();
    // `b = a;` followed by another use of `a` means two live handles to one object (shared_ptr). Without this the assignment moves `a` and later uses are rejected.
    forceVirtualSignaturesShared(program);
    promoteAliasedLocals(program);

    // Needs the pointer kind each pushed variable already got from the two-pass decision.
    scanContainerElementKinds(program);

    // Parameter-storage promotion needs a field's decided kind; call-site propagation runs after it and needs the parameters it promoted (guide chapter 4).
    promoteParamsStoredInSharedFields(program);
    promoteSharedCallArguments(program);
    // Runs last: needs every local's final pointer kind, including what promoteSharedCallArguments just changed.
    promoteReturnsOfSharedLocals(program);
    // Needs every class method's final alias-return classification to classify a free function that calls one.
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

PointerType SmartPointerAnalyzer::getPointerTypeInScope(const std::string& scopeName,
                                                        const std::string& className,
                                                        const std::string& variableName) const {
    if (!scopeName.empty()) {
        auto it = pointerDecisions.find(buildKey(scopeName, className, variableName));
        if (it != pointerDecisions.end()) return it->second;
    }
    return getPointerType(className, variableName);
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

bool SmartPointerAnalyzer::isWeakField(const std::string& enclosingClass, const std::string& fieldName) const {
    // pointerDecisions keys are "Scope|Type.name"; for a field the scope is the enclosing class, so match on scope prefix and ".name" suffix.
    const std::string prefix = enclosingClass + "|";
    const std::string suffix = "." + fieldName;
    for (const auto& [key, decision] : pointerDecisions) {
        if (key.size() > prefix.size() + suffix.size() && key.compare(0, prefix.size(), prefix) == 0 &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return decision == PointerType::WEAK;
        }
    }
    return false;
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
    pushedArgs_.clear();
    scanContainerElementKindsImpl(program);
    for (const std::string& name : demandedElements_) {
        // only a container of objects this program declares (containers are told apart by name alone)
        auto type = containerFieldTypes_.find(name);
        if (type != containerFieldTypes_.end() && isVectorType(type->second) && classNames.count(vectorElementType(type->second)) > 0) {
            containerElementShared_[name] = true;
        }
    }
    // A value pushed into a container whose elements are shared must itself be shared; decided once every push and element copy has been seen.
    for (const auto& p : pushedArgs_) {
        auto it = containerElementShared_.find(p[3]);
        if (it == containerElementShared_.end() || !it->second) continue;
        auto dec = pointerDecisions.find(buildKey(p[0], p[1], p[2]));
        if (dec != pointerDecisions.end() && dec->second == PointerType::UNIQUE) {
            dec->second = PointerType::SHARED;
            pointerReasons[dec->first] = "pushed into a container whose elements are shared";
        }
    }
}

namespace {
void collectDeclaredNames(const std::vector<StatementPtr>& body, std::set<std::string>& names) {
    for (const auto& st : body) {
        if (auto d = dynamic_cast<const VariableDeclaration*>(st.get())) {
            names.insert(d->identifier);
        } else if (auto c = dynamic_cast<const ContainerDeclaration*>(st.get())) {
            names.insert(c->identifier);
        } else if (auto i = dynamic_cast<const IfStatement*>(st.get())) {
            collectDeclaredNames(i->body, names);
            collectDeclaredNames(i->elseBody, names);
        } else if (auto w = dynamic_cast<const WhileStatement*>(st.get())) {
            collectDeclaredNames(w->body, names);
        } else if (auto f = dynamic_cast<const ForStatement*>(st.get())) {
            names.insert(f->loopVarName);
            collectDeclaredNames(f->body, names);
        } else if (auto t = dynamic_cast<const TryStatement*>(st.get())) {
            names.insert(t->catchIdentifier);
            collectDeclaredNames(t->body, names);
            collectDeclaredNames(t->catchBody, names);
        }
    }
}
}  // namespace

void SmartPointerAnalyzer::scanContainerElementKindsImpl(const Program& program) {
    containerFieldTypes_.clear();
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const ContainerDeclaration*>(member.get())) {
                    containerFieldTypes_[field->identifier] = field->type;
                } else if (auto plain = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    if (isVectorType(plain->type)) containerFieldTypes_[plain->identifier] = plain->type;     // (a field `std::vector<Cmd> items;`)
                }
            }
        }
    }
    auto scanBody = [this](const std::vector<StatementPtr>& body,
                           const std::vector<std::pair<std::string, std::string>>& params, const std::string& scope) {
        scanLocals_.clear();
        collectDeclaredNames(body, scanLocals_);
        for (const auto& p : params) scanLocals_.insert(p.second);
        for (const auto& inner : body) scanContainerElementKindsInStatement(*inner, scope);
    };
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            scanBody(fn->body, fn->parameters, fn->name);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            scanBody(mn->body, {}, "main");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    scanBody(method->body, method->parameters, cls->name + "::" + method->name);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    scanBody(ctor->body, ctor->parameters, cls->name + "::" + ctor->name);
                }
            }
        }
    }
    scanLocals_.clear();
}

namespace {
// The identifier named by `expr`, if it is a bare identifier (the shape `container.push_back(sourceVar)` needs).
const Identifier* asIdentifier(const Expression* expr) {
    return dynamic_cast<const Identifier*>(expr);
}
} // namespace

void SmartPointerAnalyzer::scanContainerElementKindsInStatement(const Statement& stmt,
                                                                  const std::string& scopeName) {
    // Find every `<container>.push_back(<arg>)`, wherever it appears.
    auto visitExpr = [this, &scopeName](const Expression* expr) {
        auto call = dynamic_cast<const FunctionCall*>(expr);
        if (call == nullptr || !call->callee) return;
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr || member->member != "push_back" || call->arguments.empty()) return;
        const Identifier* recvId = asIdentifier(member->object.get());
        const Identifier* arg = asIdentifier(call->arguments[0].get());
        // the container is a variable (`v.push_back(x)`) or a field reached through an object (`obj.items.push_back(x)`, keyed by the field's name)
        const MemberAccess* recvField = dynamic_cast<const MemberAccess*>(member->object.get());
        if ((recvId == nullptr && recvField == nullptr) || arg == nullptr) return;
        const std::string recvName = recvId != nullptr ? recvId->name : recvField->member;

        // The argument's already-decided pointer kind, looked up by its symbol type (precise scope first, then the global fallback).
        auto typeIt = symbolTypesByScope.find(makeSymbolKey(scopeName, arg->name));
        if (typeIt == symbolTypesByScope.end() && scanLocals_.count(arg->name) == 0) {
            typeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, arg->name));
        }
        if (typeIt == symbolTypesByScope.end()) return;
        // Only an object argument says anything about the element kind; the not-found fallback is shared, so a string or int would wrongly make a vector of shared objects.
        if (classNames.count(typeIt->second) == 0) return;
        pushedArgs_.push_back({scopeName, typeIt->second, arg->name, recvName});
        if (getPointerType(typeIt->second, arg->name) == PointerType::SHARED) {
            containerElementShared_[recvName] = true;
        } else if (containerElementShared_.find(recvName) == containerElementShared_.end()) {
            containerElementShared_[recvName] = false;
        }
    };

    if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(&stmt)) {
        visitExpr(exprStmt->expression.get());
    } else if (auto decl = dynamic_cast<const VariableDeclaration*>(&stmt)) {
        if (decl->expression) visitExpr(decl->expression.get());
        // `Cmd c = items[i];`: two owners of one object, so the elements must be shared.
        if (!decl->isObserver && classNames.count(decl->type) > 0) {
            if (auto sub = dynamic_cast<const SubscriptExpression*>(decl->expression.get())) {
                if (auto cid = dynamic_cast<const Identifier*>(sub->object.get())) {
                    containerElementShared_[cid->name] = true;
                } else if (auto cmem = dynamic_cast<const MemberAccess*>(sub->object.get())) {
                    containerElementShared_[cmem->member] = true;   // `obj.items[i]`: the container is the field `items`
                }
            }
        }
    } else if (auto assign = dynamic_cast<const AssignmentStatement*>(&stmt)) {
        if (assign->expression) visitExpr(assign->expression.get());
        // `v[i] = v[j];` or `x = v[i];`: two owners of one object, so shared elements. The container is known to hold objects from its declared type, not from a push_back.
        if (auto from = dynamic_cast<const SubscriptExpression*>(assign->expression.get())) {
            auto nameOf = [](const Expression* object) -> std::string {
                if (auto id = dynamic_cast<const Identifier*>(object)) return id->name;
                if (auto field = dynamic_cast<const MemberAccess*>(object)) return field->member;
                return "";
            };
            auto holdsObjects = [this](const std::string& container) {
                auto type = containerFieldTypes_.find(container);
                return type != containerFieldTypes_.end() && isVectorType(type->second) && classNames.count(vectorElementType(type->second)) > 0;
            };
            const std::string source = nameOf(from->object.get());
            if (!source.empty() && holdsObjects(source)) containerElementShared_[source] = true;
            if (auto to = dynamic_cast<const SubscriptExpression*>(assign->target.get())) {
                const std::string destination = nameOf(to->object.get());
                if (!destination.empty() && holdsObjects(destination)) containerElementShared_[destination] = true;
            }
        }
    } else if (auto ifs = dynamic_cast<const IfStatement*>(&stmt)) {
        for (const auto& inner : ifs->body) scanContainerElementKindsInStatement(*inner, scopeName);
        for (const auto& inner : ifs->elseBody) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto whiles = dynamic_cast<const WhileStatement*>(&stmt)) {
        for (const auto& inner : whiles->body) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto fors = dynamic_cast<const ForStatement*>(&stmt)) {
        for (const auto& inner : fors->body) scanContainerElementKindsInStatement(*inner, scopeName);
    } else if (auto trys = dynamic_cast<const TryStatement*>(&stmt)) {
        for (const auto& inner : trys->body) scanContainerElementKindsInStatement(*inner, scopeName);
        for (const auto& inner : trys->catchBody) scanContainerElementKindsInStatement(*inner, scopeName);
    }
}

namespace {
// Every `return <expr>;` reachable from `body`, retested on each fixed-point round below.
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
        } else if (auto trys = dynamic_cast<const TryStatement*>(stmtPtr.get())) {
            collectReturnExpressions(trys->body, out);
            collectReturnExpressions(trys->catchBody, out);
        }
    }
}

// True if `this` appears in `body` as a call argument, a declaration RHS or an assignment RHS, at any nesting depth: the three positions AstEmitter::emitMovedArg
// lowers to shared_from_this(). `return this;` is covered by the anyReturnsThis scan.
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
                // `return this;` is the anyReturnsThis scan's job; this only catches `this` nested in a larger returned expression.
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
// Resolves an expression inside `new SameClass(<expr>)` (case (d) of inferAliasReturningMethods) to the (declaredType, fieldName) it reads, crossing classes via
// classMembersByClass_. Handles a bare field/`this->field`, or `<chain>.<method>(...)` with the method already alias-returning on the chain's class. nullopt for
// anything else (fails closed).
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
        // `this->field` / `recv.field`: resolve the receiver's class (bare identifier receiver only), then look up the member in that class.
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
            // `a.b.c`: resolve `a.b`'s class first.
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
        // Resolve the receiver chain's class; the method must already be alias-returning on it.
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
        // The result aliases the receiver's own resolved field, so recurse into the receiver.
        return resolveInnerFieldChain(callMember->object.get(), enclosingClass, classMembersByClass,
                                       methodReturnKinds);
    }
    return std::nullopt;
}
} // namespace

void SmartPointerAnalyzer::inferAliasReturningMethods(const Program& program) {
    // A method is alias-returning (returns shared_ptr<T>) iff every return in its body is one of:
    //   (a) `return this;`
    //   (b) `return <field>;` naming an object-typed field of the same class
    //   (c) `return <recv>.<method>(...);` where the method is already alias-returning
    //   (d) `return new SameClass(<inner>);` (see below)
    // Case (c) makes this a fixed point: leaf methods (a/b) are classified first. Classification only goes from undecided to decided, so it terminates.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;

        // className.fieldName -> is this field object-typed (case (b) needs an object-typed field).
        std::map<std::string, std::string> fieldTypes;
        for (const auto& member : cls->members) {
            if (auto var = dynamic_cast<const VariableDeclaration*>(member.get())) {
                fieldTypes[var->identifier] = var->type;
            }
        }

        std::vector<const MethodDeclaration*> methods;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                // Specializations are skipped: the sets and maps here are keyed by bare method name, so a specialization sharing a name would corrupt the primary's
                // classification. Their return lowering is handled elsewhere (AstEmitter::currentSpecializationValueType_).
                if (!method->specializationArg.empty()) continue;
                methods.push_back(method);
            }
        }

        std::set<std::string> aliasReturning;     // method names decided ALIAS_RETURNING this round
        std::set<std::string> notAliasReturning;  // method names decided NOT this round
        std::set<std::string> fieldsNeedingShared;
        // Case (d)'s fields carry their own declared type (possibly of another class), so they are kept apart from fieldsNeedingShared.
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
                        // `this->field` / `this.field` (a field access, not the whole return value: that is case (a))
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
                        // case (c): `<recv>.<method>(...)` or a bare `<method>(...)` with an implicit `this`; either way the called method's classification decides.
                        std::string calledName;
                        if (auto callMember = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                            calledName = callMember->member;
                        } else if (!call->callee) {
                            calledName = call->function_name;
                        }
                        // Self-recursion cannot block its own classification: if every other return matches, the recursive call is consistent with it. Otherwise a method
                        // that only calls itself would wait forever and default to not alias-returning.
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
                        // Callee not yet classified: retry on a later round if it names a same-class method; otherwise this return can never resolve to alias.
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
                    // case (d): `return new T(<inner>);` where T is the method's own class (a handle wrapping a payload, e.g. `Node::operator[]`): the aliased field belongs to
                    // another class's chain (resolveInnerFieldChain). It is recorded in fieldsNeedingShared under its own declared type, which is how explicitHints is keyed.
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
                // still depends on an undecided sibling method: leave unclassified, retry next round.
            }
        }
        // Anything still unclassified after the loop stabilizes (a cycle of never-resolving methods) defaults to not alias-returning.

        // Also register under the qualified name (`YAML::Node` as well as `Node`): lookups in AstEmitter/DanglingChecker key by the receiver's declared type string.
        std::vector<std::string> classKeys = {cls->name};
        if (!cls->namespaceName.empty()) {
            classKeys.push_back(cls->namespaceName + "::" + cls->name);
        }
        for (const auto& name : aliasReturning) {
            for (const auto& classKey : classKeys) {
                methodReturnKinds_[makeMemberKey(classKey, name)] = PointerType::SHARED;
            }
        }
        // A field returned by an alias-returning method must be shared like any other field lookup (getPointerType consults explicitHints). The key is
        // "<declared type of the field>.<field name>", not the containing class: that is what secondPassDeterminePointerTypes queries. explicitHintLines is not
        // touched, so inferred entries never trigger the "hint didn't match" diagnostic.
        for (const auto& fieldName : fieldsNeedingShared) {
            explicitHints[makeMemberKey(fieldTypes[fieldName], fieldName)] = PointerType::SHARED;
        }
        // Case (d)'s fields already carry their (declaredType, fieldName), possibly of another class, so write them directly.
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
            // A value-type (struct) class's `return this;` is lowered to `return *this;` and needs no shared_ptr machinery.
            if (anyReturnsThis && !isValueTypeClass(cls->name)) {
                // A class returning `this` as a shared_ptr must always be constructed as shared_ptr (shared_from_this() needs it); reuses classesNeedingSharedRoots.
                classesNeedingSharedFromThis_.insert(cls->name);
                classesNeedingSharedRoots.insert(cls->name);
            }
        }
        // Separate scan over every method: passing `this` as a call argument or storing it (`Node y = this;`) is lowered to shared_from_this() by
        // emitMovedArg, so it needs the same enable_shared_from_this and shared-root machinery. Value-type classes are excluded as above.
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
    // Free-function counterpart of case (c): a free function that only returns a call to an already-classified alias-returning method or function is itself
    // alias-returning. Runs after promoteReturnsOfSharedLocals, because a class method such as parseDocument only becomes shared through that pass.
    std::set<std::string> aliasReturningFreeFunctions;
    // Fixed point: a function that forwards `return other(...)` is classified from `other`, which may be defined later or classified by an earlier pass.
    for (bool changed = true; changed;) {
    changed = false;
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        if (classNames.find(fn->returnType) == classNames.end()) continue;
        if (aliasReturningFreeFunctions.count(fn->name) > 0) continue;

        std::vector<const Expression*> returns;
        collectReturnExpressions(fn->body, returns);
        if (returns.empty()) continue;

        bool allAliasCalls = true;
        bool sawCall = false;
        for (const Expression* expr : returns) {
            // `return nullptr;` fits either pointer kind and says nothing about this one.
            if (auto nullId = dynamic_cast<const Identifier*>(expr); nullId != nullptr && nullId->name == "nullptr") {
                continue;
            }
            // `return x.field;` hands out an object somebody else keeps owning: shared.
            if (auto ma = dynamic_cast<const MemberAccess*>(expr)) {
                sawCall = true;
                // The returned field must itself be a shared handle: resolve `base.field` (one level, or a chain) to the field's owning class and mark that field shared.
                std::function<std::string(const Expression*)> classOfExpr = [&](const Expression* e) -> std::string {
                    if (auto id = dynamic_cast<const Identifier*>(e)) {
                        auto t = symbolTypesByScope.find(makeSymbolKey(fn->name, id->name));
                        return t == symbolTypesByScope.end() ? std::string() : t->second;
                    }
                    if (auto m = dynamic_cast<const MemberAccess*>(e)) {
                        const std::string owner = classOfExpr(m->object.get());
                        if (owner.empty()) return "";
                        auto t = symbolTypesByScope.find(makeSymbolKey(owner, m->member));
                        return t == symbolTypesByScope.end() ? std::string() : t->second;
                    }
                    return "";
                };
                const std::string owner = classOfExpr(ma->object.get());
                if (!owner.empty()) {
                    for (auto& [vkey, vref] : variableReferences) {
                        if (vref.containingScope == owner && vref.variableName == ma->member &&
                            !vref.hasExplicitHint && pointerDecisions[vkey] == PointerType::UNIQUE) {
                            pointerDecisions[vkey] = PointerType::SHARED;
                            pointerReasons[vkey] = "returned as an alias by a free function";
                            vref.reason = pointerReasons[vkey];
                        }
                    }
                }
                continue;
            }
            auto call = dynamic_cast<const FunctionCall*>(expr);
            if (call == nullptr) { allAliasCalls = false; break; }
            sawCall = true;
            std::string calledClass;
            std::string calledName;
            if (call->callee) {
                auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
                if (member == nullptr) { allAliasCalls = false; break; }
                if (auto recvId = dynamic_cast<const Identifier*>(member->object.get())) {
                    // Try this function's own scope first: the global fallback entry is last-write-wins, so two functions with a local of the same name and different types collide there.
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
                if (aliasReturningFreeFunctions.count(calledName) == 0 &&
                    getMethodReturnPointerType("", calledName) != PointerType::SHARED) {
                    allAliasCalls = false;
                    break;
                }
            } else if (getMethodReturnPointerType(calledClass, calledName) != PointerType::SHARED) {
                allAliasCalls = false;
                break;
            }
        }
        if (allAliasCalls && sawCall) {
            changed = true;
            aliasReturningFreeFunctions.insert(fn->name);
            methodReturnKinds_[makeMemberKey("", fn->name)] = PointerType::SHARED;
            // Also register under the qualified name (`YAML::Load`): a call outside the namespace spells it that way, while the definition side queries the bare name.
            if (!fn->namespaceName.empty()) {
                methodReturnKinds_[makeMemberKey("", fn->namespaceName + "::" + fn->name)] =
                    PointerType::SHARED;
            }
        }
    }
    }
}

namespace {
// The initializer expression of the local variable `name` declared somewhere in `body` (nested blocks included), or nullptr if it is not declared
// with an initializer. The first declaration wins.
const Expression* findLocalInitializer(const std::vector<StatementPtr>& body, const std::string& name) {
    for (const auto& st : body) {
        if (auto d = dynamic_cast<const VariableDeclaration*>(st.get())) {
            if (d->identifier == name) return d->expression.get();
        } else if (auto i = dynamic_cast<const IfStatement*>(st.get())) {
            if (auto found = findLocalInitializer(i->body, name)) return found;
            if (auto found = findLocalInitializer(i->elseBody, name)) return found;
        } else if (auto w = dynamic_cast<const WhileStatement*>(st.get())) {
            if (auto found = findLocalInitializer(w->body, name)) return found;
        } else if (auto f = dynamic_cast<const ForStatement*>(st.get())) {
            if (auto found = findLocalInitializer(f->body, name)) return found;
        } else if (auto t = dynamic_cast<const TryStatement*>(st.get())) {
            if (auto found = findLocalInitializer(t->body, name)) return found;
            if (auto found = findLocalInitializer(t->catchBody, name)) return found;
        }
    }
    return nullptr;
}
}  // namespace

// Is `init` a call whose result is shared, by the classification so far? The same question AstEmitter::isSharedReturningCall answers when it declares
// the local: `recv.method(...)` (the receiver's class from its declared type), or a plain call (a free function, or a method of `ownClass`).
bool SmartPointerAnalyzer::localComesFromSharedCall(const Expression* init, const std::string& ownClass, const std::string& scope) const {
    auto call = dynamic_cast<const FunctionCall*>(init);
    if (call == nullptr || !call->templateArg.empty()) return false;
    if (call->callee) {
        auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
        if (member == nullptr) return false;
        auto recv = dynamic_cast<const Identifier*>(member->object.get());
        if (recv == nullptr) return false;
        auto type = symbolTypesByScope.find(makeSymbolKey(scope, recv->name));
        if (type == symbolTypesByScope.end()) type = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, recv->name));
        if (type == symbolTypesByScope.end()) return false;
        return getMethodReturnPointerType(type->second, member->member) == PointerType::SHARED;
    }
    if (getMethodReturnPointerType("", call->function_name) == PointerType::SHARED) return true;
    return !ownClass.empty() && getMethodReturnPointerType(ownClass, call->function_name) == PointerType::SHARED;
}

void SmartPointerAnalyzer::promoteReturnsOfSharedLocals(const Program& program) {
    // `Item w = get(i); ... return w;`: a local initialised from a call whose result is shared is shared (as AstEmitter::isSharedReturningCall declares it), so the
    // method must return shared too. Which methods return shared depends on this pass itself, so it runs to a fixed point.
    for (bool changed = true; changed;) {
        changed = false;
        promoteReturnsOfSharedLocalsOnce(program, changed);
    }
}

void SmartPointerAnalyzer::promoteReturnsOfSharedLocalsOnce(const Program& program, bool& changed) {
    // inferAliasReturningMethods is structural and does not recognize `return <local>;`. A local can become shared in a later pass (promoteSharedCallArguments),
    // and the method returning it must then return shared. This pass runs after that one and reuses getPointerTypeForVariable.
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr) continue;
            // Specializations are skipped: methodReturnKinds_ is keyed by bare method name, so a specialization would corrupt the primary's entry.
            if (!method->specializationArg.empty()) continue;
            // Only for a class-typed return: getPointerTypeForVariable defaults to shared for a name it cannot match, which would misfire on a primitive local.
            if (classNames.find(method->returnType) == classNames.end()) continue;
            const std::string key = makeMemberKey(cls->name, method->name);
            if (methodReturnKinds_.count(key) > 0) continue;  // already SHARED

            std::vector<const Expression*> returns;
            collectReturnExpressions(method->body, returns);
            bool anyLocalIsShared = false;
            // Scoped lookup ("Class::method"): a plain name-only match would collide with a same-named local in an unrelated function.
            const std::string methodScope = cls->name + "::" + method->name;
            for (const Expression* expr : returns) {
                // `return this;` is handled by the anyReturnsThis / classesNeedingSharedFromThis_ mechanism; ThisExpression is not an Identifier, so it falls through here.
                auto id = dynamic_cast<const Identifier*>(expr);
                if (id == nullptr) continue;
                if (getPointerTypeForVariableInScope(methodScope, id->name) == PointerType::SHARED ||
                    localComesFromSharedCall(findLocalInitializer(method->body, id->name), cls->name, methodScope)) {
                    anyLocalIsShared = true;
                    break;
                }
            }
            if (anyLocalIsShared) {
                changed = true;
                methodReturnKinds_[key] = PointerType::SHARED;
                if (!cls->namespaceName.empty()) {
                    methodReturnKinds_[makeMemberKey(cls->namespaceName + "::" + cls->name, method->name)] =
                        PointerType::SHARED;
                }
            }
        }
    }

    // Same check for top-level free functions, keyed under the "" class (as emitFunction queries them).
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr) continue;
        if (classNames.find(fn->returnType) == classNames.end()) continue;
        const std::string key = makeMemberKey("", fn->name);
        if (methodReturnKinds_.count(key) > 0) continue;

        std::vector<const Expression*> returns;
        collectReturnExpressions(fn->body, returns);
        bool anyLocalIsShared = false;
        // Scoped lookup (bare fn->name), for the same reason as the class-method loop above.
        for (const Expression* expr : returns) {
            auto id = dynamic_cast<const Identifier*>(expr);
            if (id == nullptr) continue;
            if (getPointerTypeForVariableInScope(fn->name, id->name) == PointerType::SHARED ||
                localComesFromSharedCall(findLocalInitializer(fn->body, id->name), "", fn->name)) {
                anyLocalIsShared = true;
                break;
            }
        }
        if (anyLocalIsShared) {
            changed = true;
            // Keyed under the bare name ("" class + fn->name), as AstEmitter::buildFunctionSignature queries the definition. Also registered under the qualified name
            // (`YAML::Clone`), which is how a caller outside the namespace spells it.
            methodReturnKinds_[key] = PointerType::SHARED;
            if (!fn->namespaceName.empty()) {
                methodReturnKinds_[makeMemberKey("", fn->namespaceName + "::" + fn->name)] =
                    PointerType::SHARED;
            }
        }
    }
}

namespace {
void collectIdentifierNames(const Expression* expr, std::set<std::string>& out) {
    if (expr == nullptr) return;
    if (auto id = dynamic_cast<const Identifier*>(expr)) { out.insert(id->name); return; }
    if (auto m = dynamic_cast<const MemberAccess*>(expr)) { collectIdentifierNames(m->object.get(), out); return; }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        collectIdentifierNames(sub->object.get(), out);
        collectIdentifierNames(sub->index.get(), out);
        return;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        collectIdentifierNames(bin->left.get(), out);
        collectIdentifierNames(bin->right.get(), out);
        return;
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) { collectIdentifierNames(un->operand.get(), out); return; }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        collectIdentifierNames(call->callee.get(), out);
        for (const auto& a : call->arguments) collectIdentifierNames(a.get(), out);
        return;
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& a : neww->arguments) collectIdentifierNames(a.get(), out);
        return;
    }
}

// One entry per statement in execution (pre-)order. `used` holds the names read by the statement's own expressions (a loop's entry holds its condition or container);
// nested bodies get entries of their own.
struct FlatStmt {
    const Statement* stmt = nullptr;
    std::set<std::string> used;
    std::string declared;
    int loopEnd = -1;  // for a while/for: index of its last nested statement
};

void flattenBody(const std::vector<StatementPtr>& body, std::vector<FlatStmt>& out) {
    for (const auto& sp : body) {
        const Statement* st = sp.get();
        FlatStmt fs;
        fs.stmt = st;
        const size_t myIndex = out.size();
        if (auto decl = dynamic_cast<const VariableDeclaration*>(st)) {
            collectIdentifierNames(decl->expression.get(), fs.used);
            fs.declared = decl->identifier;
            out.push_back(fs);
        } else if (auto cdecl = dynamic_cast<const ContainerDeclaration*>(st)) {
            fs.declared = cdecl->identifier;
            out.push_back(fs);
        } else if (auto asg = dynamic_cast<const AssignmentStatement*>(st)) {
            collectIdentifierNames(asg->expression.get(), fs.used);
            if (asg->target) collectIdentifierNames(asg->target.get(), fs.used);
            out.push_back(fs);
        } else if (auto es = dynamic_cast<const ExpressionStatement*>(st)) {
            collectIdentifierNames(es->expression.get(), fs.used);
            out.push_back(fs);
        } else if (auto ret = dynamic_cast<const ReturnStatement*>(st)) {
            collectIdentifierNames(ret->expression.get(), fs.used);
            out.push_back(fs);
        } else if (auto th = dynamic_cast<const ThrowStatement*>(st)) {
            collectIdentifierNames(th->expression.get(), fs.used);
            out.push_back(fs);
        } else if (auto ifs = dynamic_cast<const IfStatement*>(st)) {
            collectIdentifierNames(ifs->condition.get(), fs.used);
            out.push_back(fs);
            flattenBody(ifs->body, out);
            flattenBody(ifs->elseBody, out);
        } else if (auto wh = dynamic_cast<const WhileStatement*>(st)) {
            collectIdentifierNames(wh->condition.get(), fs.used);
            out.push_back(fs);
            flattenBody(wh->body, out);
            out[myIndex].loopEnd = static_cast<int>(out.size()) - 1;
        } else if (auto fo = dynamic_cast<const ForStatement*>(st)) {
            collectIdentifierNames(fo->container.get(), fs.used);
            fs.declared = fo->loopVarName;
            out.push_back(fs);
            flattenBody(fo->body, out);
            out[myIndex].loopEnd = static_cast<int>(out.size()) - 1;
        } else if (auto tr = dynamic_cast<const TryStatement*>(st)) {
            out.push_back(fs);
            flattenBody(tr->body, out);
            flattenBody(tr->catchBody, out);
        } else {
            out.push_back(fs);
        }
    }
}
} // namespace

void SmartPointerAnalyzer::promoteAliasedLocalsInBody(
    const std::vector<StatementPtr>& body,
    const std::vector<std::pair<std::string, std::string>>& params, const std::string& scope) {
    std::vector<FlatStmt> flat;
    flattenBody(body, flat);
    std::map<std::string, std::string> types;  // object-class locals and parameters
    for (const auto& [ptype, pname] : params) {
        if (classNames.count(ptype) > 0) types[pname] = ptype;
    }
    for (const auto& fs : flat) {
        if (auto decl = dynamic_cast<const VariableDeclaration*>(fs.stmt)) {
            if (classNames.count(decl->type) > 0 && !decl->isObserver) types[decl->identifier] = decl->type;
        }
    }
    auto promote = [&](const std::string& name) {
        auto ti = types.find(name);
        if (ti == types.end()) return;
        auto it = pointerDecisions.find(buildKey(scope, ti->second, name));
        if (it == pointerDecisions.end() || it->second == PointerType::WEAK) return;
        // an explicit hint always wins
        auto vr = variableReferences.find(buildKey(scope, ti->second, name));
        if (vr != variableReferences.end() && vr->second.hasExplicitHint) return;
        it->second = PointerType::SHARED;
        pointerReasons[buildKey(scope, ti->second, name)] =
            "assigned to another handle and used again afterwards (two live owners)";
    };
    for (size_t i = 0; i < flat.size(); ++i) {
        std::string target;
        const Expression* rhs = nullptr;
        if (auto decl = dynamic_cast<const VariableDeclaration*>(flat[i].stmt)) {
            if (classNames.count(decl->type) == 0 || decl->isObserver) continue;
            target = decl->identifier;
            rhs = decl->expression.get();
        } else if (auto asg = dynamic_cast<const AssignmentStatement*>(flat[i].stmt)) {
            if (asg->target) continue;  // a field store already shares
            target = asg->variable;
            rhs = asg->expression.get();
        } else {
            continue;
        }
        // `T x = v[i];` / `T x = obj.items[i];` copies a handle out of a container element: x and the element are two owners.
        if (auto sub = dynamic_cast<const SubscriptExpression*>(rhs);
            sub != nullptr && types.find(target) != types.end() &&
            (dynamic_cast<const Identifier*>(sub->object.get()) != nullptr ||
             dynamic_cast<const MemberAccess*>(sub->object.get()) != nullptr)) {
            promote(target);
            continue;
        }
        // `T x = field;` inside a method: the local and the field are two owners from now on (a field copy never moves the field). Both become shared,
        // otherwise the local would be a shared_ptr initialised from a unique_ptr field.
        if (auto fieldId = dynamic_cast<const Identifier*>(rhs);
            fieldId != nullptr && types.find(fieldId->name) == types.end() && types.find(target) != types.end()) {
            const size_t sep = scope.find("::");
            if (sep != std::string::npos) {
                const std::string cls = scope.substr(0, sep);
                auto members = classMembersByClass_.find(cls);
                if (members != classMembersByClass_.end()) {
                    auto fieldType = members->second.find(fieldId->name);
                    if (fieldType != members->second.end() && classNames.count(fieldType->second) > 0) {
                        for (auto& [key, ref] : variableReferences) {
                            if (ref.className != fieldType->second || ref.variableName != fieldId->name) continue;
                            if (ref.containingScope != normalizeScope(cls) || ref.hasExplicitHint) continue;
                            auto dec = pointerDecisions.find(key);
                            if (dec == pointerDecisions.end() || dec->second == PointerType::WEAK) continue;
                            dec->second = PointerType::SHARED;
                            pointerReasons[key] = "copied into a local handle (two live owners)";
                        }
                        promote(target);
                        continue;
                    }
                }
            }
        }
        auto rhsId = dynamic_cast<const Identifier*>(rhs);
        if (rhsId == nullptr || types.find(rhsId->name) == types.end() || types.find(target) == types.end()) {
            continue;
        }
        const std::string& src = rhsId->name;
        bool usedAgain = false;
        for (size_t j = i + 1; j < flat.size() && !usedAgain; ++j) {
            if (flat[j].used.count(src) > 0) usedAgain = true;
        }
        // inside a loop, a use anywhere in the loop is 'after' on the next iteration, unless the source is declared inside that loop
        for (size_t l = 0; l < i && !usedAgain; ++l) {
            if (flat[l].loopEnd < 0 || static_cast<size_t>(flat[l].loopEnd) < i) continue;
            bool declaredInside = false;
            for (size_t k = l; k <= static_cast<size_t>(flat[l].loopEnd); ++k) {
                if (k != l && flat[k].declared == src) declaredInside = true;
            }
            if (declaredInside) continue;
            for (size_t k = l; k <= static_cast<size_t>(flat[l].loopEnd) && !usedAgain; ++k) {
                if (k != i && flat[k].used.count(src) > 0) usedAgain = true;
            }
        }
        if (usedAgain) {
            promote(src);
            promote(target);
        }
    }
}

void SmartPointerAnalyzer::forceVirtualSignaturesShared(const Program& program) {
    for (const auto& stmt : program.statements) {
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        for (const auto& member : cls->members) {
            auto method = dynamic_cast<const MethodDeclaration*>(member.get());
            if (method == nullptr || !(method->isVirtual || method->isOverride)) continue;
            const std::string scope = cls->name + "::" + method->name;
            for (const auto& [ptype, pname] : method->parameters) {
                if (classNames.count(ptype) == 0) continue;
                const std::string key = buildKey(scope, ptype, pname);
                if (variableReferences.find(key) == variableReferences.end()) continue;
                pointerDecisions[key] = PointerType::SHARED;
                pointerReasons[key] = "parameter of a virtual / override method (signatures must agree across the hierarchy)";
            }
            if (classNames.count(method->returnType) > 0) {
                methodReturnKinds_[makeMemberKey(cls->name, method->name)] = PointerType::SHARED;
            }
        }
    }
}

void SmartPointerAnalyzer::promoteAliasedLocals(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            promoteAliasedLocalsInBody(fn->body, fn->parameters, fn->name);
        } else if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            promoteAliasedLocalsInBody(mn->body, {}, "main");
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    promoteAliasedLocalsInBody(method->body, method->parameters,
                                               cls->name + "::" + method->name);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    promoteAliasedLocalsInBody(ctor->body, ctor->parameters, cls->name + "::" + ctor->name);
                }
            }
        }
    }
}

void SmartPointerAnalyzer::promoteParamsStoredInSharedFields(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            promoteParamsInBody(fn->body, "", fn->parameters, fn->name);
            continue;
        }
        if (auto mn = dynamic_cast<const MainFunction*>(stmt.get())) {
            promoteParamsInBody(mn->body, "", {}, "main");
            continue;
        }
        auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get());
        if (cls == nullptr) continue;
        // A spawn job's constructor moves its object captures into unique fields; the usual "parameter stored into a field is linked" rule would leave the creating
        // thread an owner of an object that now runs concurrently. DanglingChecker::checkSpawnCall decides what may be moved.
        if (cls->isSpawnJob) continue;
        for (const auto& member : cls->members) {
            if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                promoteParamsInBody(method->body, cls->name, method->parameters, cls->name + "::" + method->name);
            } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                promoteParamsInBody(ctor->body, cls->name, ctor->parameters, cls->name + "::" + ctor->name);
            }
        }
    }
}

void SmartPointerAnalyzer::promoteParamsInBody(const std::vector<StatementPtr>& body,
                                               const std::string& className,
                                               const std::vector<std::pair<std::string, std::string>>& params,
                                               const std::string& scope) {
    // A store is recognized as `field = param;` (bare field name, or through another object: `array.child = item;`) anywhere in a body, including inside
    // if/while/for/try and in free functions. Promotion is always safe (shared only means the caller's variable is copied instead of moved). The field's
    // owning class is resolved from the target's base expression (parameter, local or field chain), else the enclosing class.
    std::map<std::string, std::string> localTypes;
    for (const auto& [ptype, pname] : params) localTypes[pname] = ptype;
    std::function<void(const std::vector<StatementPtr>&)> collectLocals =
        [&](const std::vector<StatementPtr>& stmts) {
            for (const auto& sp : stmts) {
                if (auto decl = dynamic_cast<const VariableDeclaration*>(sp.get())) {
                    if (localTypes.find(decl->identifier) == localTypes.end()) {
                        localTypes[decl->identifier] = decl->type;
                    }
                } else if (auto ifs = dynamic_cast<const IfStatement*>(sp.get())) {
                    collectLocals(ifs->body);
                    collectLocals(ifs->elseBody);
                } else if (auto whiles = dynamic_cast<const WhileStatement*>(sp.get())) {
                    collectLocals(whiles->body);
                } else if (auto fors = dynamic_cast<const ForStatement*>(sp.get())) {
                    collectLocals(fors->body);
                } else if (auto trys = dynamic_cast<const TryStatement*>(sp.get())) {
                    collectLocals(trys->body);
                    collectLocals(trys->catchBody);
                }
            }
        };
    collectLocals(body);
    std::function<std::string(const Expression*)> classOf = [&](const Expression* e) -> std::string {
        if (auto id = dynamic_cast<const Identifier*>(e)) {
            auto it = localTypes.find(id->name);
            if (it != localTypes.end()) return it->second;
            if (!className.empty()) {
                auto f = symbolTypesByScope.find(makeSymbolKey(className, id->name));
                if (f != symbolTypesByScope.end()) return f->second;
            }
            return "";
        }
        if (auto m = dynamic_cast<const MemberAccess*>(e)) {
            const std::string owner = classOf(m->object.get());
            if (owner.empty()) return "";
            auto f = symbolTypesByScope.find(makeSymbolKey(owner, m->member));
            return f == symbolTypesByScope.end() ? std::string() : f->second;
        }
        return "";
    };
    std::function<void(const std::vector<StatementPtr>&)> visit;
    visit = [&](const std::vector<StatementPtr>& stmts) {
    for (const auto& stmtPtr : stmts) {
        if (auto ifs = dynamic_cast<const IfStatement*>(stmtPtr.get())) {
            visit(ifs->body);
            visit(ifs->elseBody);
            continue;
        }
        if (auto whiles = dynamic_cast<const WhileStatement*>(stmtPtr.get())) {
            visit(whiles->body);
            continue;
        }
        if (auto fors = dynamic_cast<const ForStatement*>(stmtPtr.get())) {
            visit(fors->body);
            continue;
        }
        if (auto trys = dynamic_cast<const TryStatement*>(stmtPtr.get())) {
            visit(trys->body);
            visit(trys->catchBody);
            continue;
        }
        auto assign = dynamic_cast<const AssignmentStatement*>(stmtPtr.get());
        if (assign == nullptr) continue;

        std::string fieldName;
        std::string fieldOwner = className;  // class whose field is being stored into
        if (!assign->target) {
            if (className.empty()) continue;  // a plain local assignment in a free function
            fieldName = assign->variable;  // bare identifier, implicit `this`
        } else if (auto member = dynamic_cast<const MemberAccess*>(assign->target.get())) {
            fieldName = member->member;
            const std::string resolved = classOf(member->object.get());
            if (!resolved.empty()) fieldOwner = resolved;
            if (fieldOwner.empty()) continue;
        } else {
            continue;
        }

        auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
        if (rhsId == nullptr) continue;

        // RHS must be one of this body's own parameters, or (when the field is already shared) a local declared in it: a local stored into a shared field must be
        // shared, or `field = local` assigns a unique_ptr lvalue to a shared_ptr.
        const bool rhsIsParam = std::any_of(params.begin(), params.end(),
            [&](const auto& p) { return p.second == rhsId->name; });
        const bool rhsIsLocal = !rhsIsParam && localTypes.count(rhsId->name) > 0;
        if (!rhsIsParam && !rhsIsLocal) continue;

        // The field's declared type tells which (class, name) pair to ask getPointerType about.
        const auto fieldTypeIt = symbolTypesByScope.find(makeSymbolKey(fieldOwner, fieldName));
        if (fieldTypeIt == symbolTypesByScope.end()) continue;

        std::pair<std::string, std::string> paramTypeEntry;
        if (rhsIsParam) {
            auto pit = std::find_if(params.begin(), params.end(),
                [&](const auto& p) { return p.second == rhsId->name; });
            paramTypeEntry = *pit;
        } else {
            paramTypeEntry = {localTypes[rhsId->name], rhsId->name};
        }
        auto paramTypeIt = &paramTypeEntry;
        if (classNames.find(paramTypeIt->first) == classNames.end()) continue;
        if (rhsIsLocal && getPointerType(fieldTypeIt->second, fieldName) != PointerType::SHARED) continue;

        if (getPointerType(fieldTypeIt->second, fieldName) != PointerType::SHARED) {
            // The field was not decided shared by anything, most commonly by secondPassDeterminePointerTypes' base case ("default unique ownership"). An inline
            // `new` passed as a constructor argument is emitted with getPointerType's not-found default, which is shared, so a unique field would not accept it.
            // Make the field agree with that default. `pointerDecisions` has an entry for every key, so the reason string is the only way to tell the base case
            // from a real rule (hint, back-reference, bidirectional, shared-root, data-flow); a real rule is not overridden.
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

        // The parameter's declared type gives the (class, name) key to promote; registerParameters gave every object-typed parameter a variableReferences entry.
        for (auto& [key, ref] : variableReferences) {
            if (ref.className == paramTypeIt->first && ref.variableName == rhsId->name &&
                (rhsIsParam || ref.containingScope == normalizeScope(scope))) {
                pointerDecisions[key] = PointerType::SHARED;
                pointerReasons[key] = "parameter is stored into an already-shared field";
                ref.reason = pointerReasons[key];
            }
        }
    }
    };
    visit(body);
}

namespace {
// Transient lookup built once per promoteSharedCallArguments call: a callee (free function name, or "ClassName::methodName") to its declared parameter list.
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
            // Also register under the qualified name ("YAML::Clone"): a call outside the namespace spells it that way (FunctionCall::function_name is verbatim).
            if (!fn->namespaceName.empty()) {
                out.freeFunctions[fn->namespaceName + "::" + fn->name] = fn->parameters;
            }
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    // Specializations are excluded: out.methods is keyed by bare "Class::method", and a plain call never resolves to a specialization, which would only overwrite the primary's entry.
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
    // The precise scope (free function name, "main", or "Class::method") of the body visitStmt/visitExpr are walking. Every symbolTypesByScope lookup tries it
    // before the kGlobalScope fallback, which is last-write-wins and collides when unrelated functions declare a same-named local of different types.
    std::string currentScope;

    // A free function named `begin`/`end` taking one class-typed parameter exists to support ADL range-for from other .clg files. The forcing further down
    // only sees a `for` in the same file, and each .clg is compiled separately, so the parameter is forced shared here unconditionally.
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

    // A method `operator==`/`operator!=` whose single parameter is the class itself serves comparison sites anywhere (AstEmitter routes `a == b` through it).
    // Callers compare shared_ptr locals, so the parameter is forced shared whatever this file's own usage shows.
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

    // Same cross-file reason, for every free-function parameter classified read-only (classifyReadOnlyFreeFunctionParams): the per-call-site forcing below only
    // sees calls in this file, but a read-only function is read-only whatever its callers do, so force it here unconditionally.
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

    // "ClassName" -> parameter name of that class's handle-rebind `operator=(SameClass other)` overload, if declared. Kept apart from calleeParams.methods, which
    // collapses overloads to one entry per class (a class with both `operator=(string)` and `operator=(SameClass)` would lose one). Mirrors AstEmitter::operatorAssignParamTypes_.
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

    // Resolve a call or constructor's target parameter list and promote every argument bound to an already-shared parameter, so the caller's variable is
    // shared too. Arguments are resolved lightly (bare identifier, via symbolTypesByScope).
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
                // `recv.method(args)`: resolve recv's class, then the method's parameter list.
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

                    // A read-only parameter (isReadOnlyFreeFunctionParam) must receive the caller's argument without consuming it. Both the parameter and the argument are
                    // forced shared, since a shared_ptr argument cannot bind to a unique_ptr parameter. buildKey needs the callee's bare name (parameters are scoped under
                    // the unqualified function name), so a qualified `call->function_name` is stripped.
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
            // `out << x` is a BinaryExpression, not a FunctionCall, so the call-site forcing above never sees it. A class-typed right operand streamed into a read-only
            // `operator<<` parameter needs the same promotion. Only a bare Identifier right operand is handled. `<<` is also integer shift, but promotion requires a
            // free `operator<<` whose second parameter is a known class and an operand of that type, so shifts fall through.
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
                            // Strip a qualified rhs type ("YAML::Node") to its simple name before comparing with operator<<'s bare paramType.
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
            // `a == b` / `a != b` on the same class with its own operator==/!=: AstEmitter routes it through `a->operator==(b)`, which is not a FunctionCall, so the
            // caller's right operand needs forcing here too (the method's own parameter is forced shared near the top).
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
            // Same-class `n = otherNode;` handle-rebind is handled by forceHandleRebindAssignments below, which needs class/method scope: an ordinary loop-advancement
            // local (`cur = cur.nextSibling();`) has the same shape but must stay a plain repoint.
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
            // Range-for over a class-typed container using ADL `begin(container)` / `end(container)` calls both free functions in the generated C++, invisible to the
            // walk below. The container variable and both functions' parameters are forced shared: they take the container by value twice, which a unique_ptr cannot do.
            if (auto containerId = dynamic_cast<const Identifier*>(fors->container.get())) {
                auto classIt = currentScope.empty() ? symbolTypesByScope.end()
                    : symbolTypesByScope.find(makeSymbolKey(currentScope, containerId->name));
                if (classIt == symbolTypesByScope.end()) {
                    classIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, containerId->name));
                }
                if (classIt != symbolTypesByScope.end()) {
                    const std::string& containerClass = classIt->second;
                    // Strip a qualified container type ("YAML::Node") to its simple name before comparing with a bare parameter type.
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

    // A separate walk for implicit-this calls to a read-only sibling method (`parseBlockMapInto(target, ...)`): the callee is null, like a free call, but
    // calleeParams.freeFunctions has no entry for a method. Mirrors the free-function forcing above, keyed by (class, method).
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

    // `n = otherNode;` handle-rebind forcing: the emitter routes a same-class assignment through the user's `operator=` only when the LHS is a parameter already
    // classified read-only, so an ordinary local (`cur = cur.nextSibling();`) must not be forced shared here. Scoped per (class, method) to query
    // isReadOnlyMethodParam / isReadOnlyFreeFunctionParam.
    std::function<void(const std::vector<StatementPtr>&, const std::string&, const std::string&)>
        forceHandleRebindAssignmentsInBody = [&](const std::vector<StatementPtr>& body,
                                                  const std::string& currentClass,
                                                  const std::string& currentFunctionName) {
            for (const auto& stmtPtr : body) {
                const Statement* stmt = stmtPtr.get();
                if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                    auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
                    if (rhsId == nullptr) continue;
                    // currentFunctionName is scoped precisely ("Class::method" or the bare name), so try it before the kGlobalScope fallback.
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
        // Two routes into valueTypeClasses_: structural inference (a class with the iterator protocol operator*/operator++/operator!=) or an explicit `struct`
        // (ClassDeclaration::isValueType, set at parse time).
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
    // A known-external class has no ClassDeclaration, so it is added to valueTypeClasses_ here; `new Eigen::MatrixXd(...)` then emits as a bare constructor
    // call. Covers every `class` fact of every loaded fact pack.
    for (const std::string& external : knownExternalClassTypes()) {
        valueTypeClasses_.insert(external);
    }
}

bool SmartPointerAnalyzer::isValueTypeClass(const std::string& className) const {
    return valueTypeClasses_.find(className) != valueTypeClasses_.end();
}

bool SmartPointerAnalyzer::paramUseDisqualifiesReadOnly(
    const Expression* expr, const std::string& paramName, const std::string& currentClass,
    const std::set<std::string>& okCallees) const {
    if (expr == nullptr) return false;
    // A bare occurrence of paramName is what move detection keys on; `paramName.field` is a MemberAccess and never disqualifies (its object is not recursed into).
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return id->name == paramName;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        // `paramName == x` / `!= x` compares identity; it reads the smart pointer without consuming or storing it. Only a bare paramName operand is exempt.
        if (bin->operator_str == "==" || bin->operator_str == "!=") {
            auto isBareParam = [&](const Expression* e) {
                auto id = dynamic_cast<const Identifier*>(e);
                return id != nullptr && id->name == paramName;
            };
            const bool leftOk = isBareParam(bin->left.get()) ||
                !paramUseDisqualifiesReadOnly(bin->left.get(), paramName, currentClass, okCallees);
            const bool rightOk = isBareParam(bin->right.get()) ||
                !paramUseDisqualifiesReadOnly(bin->right.get(), paramName, currentClass, okCallees);
            return !(leftOk && rightOk);
        }
        return paramUseDisqualifiesReadOnly(bin->left.get(), paramName, currentClass, okCallees) ||
               paramUseDisqualifiesReadOnly(bin->right.get(), paramName, currentClass, okCallees);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        // `!paramName` (null check) reads and does not consume. A nested use inside the operand is still checked.
        if (un->operator_str == "!") {
            auto operandId = dynamic_cast<const Identifier*>(un->operand.get());
            if (operandId != nullptr && operandId->name == paramName) return false;
        }
        return paramUseDisqualifiesReadOnly(un->operand.get(), paramName, currentClass, okCallees);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // A bare paramName passed as a call argument disqualifies unless the callee's parameter at that position is already read-only (`okCallees`, a fixed
        // point over free functions and methods). `!call->callee` covers a free call and an implicit-this method call; with `currentClass` set the method key is
        // tried first (a same-class bare call resolves to a sibling method), then the free-function key.
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
                // Bare paramName as the receiver of a method call is a read, not a move; only a nested receiver expression is descended into.
                if (auto recvMember = dynamic_cast<const MemberAccess*>(member->object.get())) {
                    return paramUseDisqualifiesReadOnly(recvMember, paramName, currentClass, okCallees);
                }
            }
        }
        return false;
    }
    // MemberAccess, SubscriptExpression, NewExpression and literals never disqualify on their own (a MemberAccess's object is not recursed into: that is the read-only access).
    return false;
}

void SmartPointerAnalyzer::classifyReadOnlyFreeFunctionParams(const Program& program) {
    // A parameter never used has no disqualifying use, but that is not evidence of read-only, and treating it so would break the ordinary move of its argument.
    // At least one genuine read of `paramName.field` (or a null check on the bare parameter) is required.
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
        // `paramName[index]` is as genuine a read as `paramName.field`.
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
            if (bin->operator_str == "==" || bin->operator_str == "!=") {
                for (const Expression* side : {bin->left.get(), bin->right.get()}) {
                    auto sideId = dynamic_cast<const Identifier*>(side);
                    if (sideId != nullptr && sideId->name == paramName) return true;
                }
            }
            return exprHasGenuineRead(bin->left.get(), paramName, currentClass, okCallees) ||
                   exprHasGenuineRead(bin->right.get(), paramName, currentClass, okCallees);
        }
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            // A bare paramName passed to an already read-only callee parameter (okCallees) counts as a genuine read too, mirroring paramUseDisqualifiesReadOnly. Method-scoped key first.
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

    // Statement-level walk over a body for any use of paramName that would require it to be consumed (it can be an assignment or call argument, not only a
    // return). `okCallees` lets a fixed-point round re-check with a growing set of read-only callees.
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

    // Fixed point over all free functions and class methods: a function passing paramName bare into another's already read-only parameter is itself read-only,
    // but the target may only be classified on a later round. Bounded by the number of functions and methods, since each round must classify a new pair.
    std::set<std::string> classified;         // "fnName::paramName", free functions
    std::set<std::string> classifiedMethods;  // "ClassName::methodName::paramName"
    bool changed = true;
    while (changed) {
        changed = false;
        // okCallees: "fnName::argIndex" for a read-only free-function parameter, "ClassName::methodName::argIndex" for a method parameter. Built each round from the
        // classified sets, so a newly classified callee is visible from the next round.
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

                    // Only object-typed parameters are a move-vs-share question.
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
                    // Specializations are excluded: classifiedKey is keyed by bare method name, and a specialization's out-parameter has its own mechanism (outParamIndex_).
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
    // Also register under the qualified name; isReadOnlyFreeFunctionParam is queried with the call site's spelling, qualified from outside the namespace.
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

bool SmartPointerAnalyzer::paramUseDisqualifiesReadOnlyContainer(
    const Expression* expr, const std::string& paramName, const std::string& currentClass,
    const std::set<std::string>& okContainerCallees) const {
    if (expr == nullptr) return false;
    // A bare occurrence of paramName disqualifies unless its context (subscript read, .size() receiver, range-for container, read-only callee argument) clears it
    // earlier; this function is only called on positions that are not such a context.
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return id->name == paramName;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        return paramUseDisqualifiesReadOnlyContainer(bin->left.get(), paramName, currentClass, okContainerCallees) ||
               paramUseDisqualifiesReadOnlyContainer(bin->right.get(), paramName, currentClass, okContainerCallees);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        return paramUseDisqualifiesReadOnlyContainer(un->operand.get(), paramName, currentClass, okContainerCallees);
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        // A subscript read (`paramName[i]` as a value) never disqualifies; only a nested object and the index are descended into. A subscript target is handled by the caller.
        if (auto id = dynamic_cast<const Identifier*>(sub->object.get()); id != nullptr && id->name == paramName) {
            return paramUseDisqualifiesReadOnlyContainer(sub->index.get(), paramName, currentClass,
                                                          okContainerCallees);
        }
        return paramUseDisqualifiesReadOnlyContainer(sub->object.get(), paramName, currentClass,
                                                      okContainerCallees) ||
               paramUseDisqualifiesReadOnlyContainer(sub->index.get(), paramName, currentClass, okContainerCallees);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        // `paramName.method(...)` -- the receiver.
        if (call->callee) {
            if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                if (auto recvId = dynamic_cast<const Identifier*>(member->object.get());
                    recvId != nullptr && recvId->name == paramName) {
                    // `.size()` is a genuine read. Every other method on a container either mutates (push_back/pop_back/resize), or is begin()/end() (an iterator can outlive a
                    // reallocation), so it disqualifies. `reserve` is treated the same way, conservatively.
                    if (member->member == "size") return false;
                    return true;
                }
                // A nested receiver (`paramName.field.method()`) is not descended into: reading a field through paramName never disqualifies.
            }
        }
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
            if (argId != nullptr && argId->name == paramName && !call->callee) {
                // Bare paramName passed to a function's parameter at this position is fine if that parameter is already read-only-container (fixed point, same keys as
                // classifyReadOnlyContainerParams) or an ext_fn `ref` parameter (ext_fn refParams are registered into the same free-function parameter list at parse
                // time, see ExternFunctionDeclaration in ast.h).
                const std::string methodKey =
                    currentClass + "::" + call->function_name + "::" + std::to_string(i);
                const std::string freeKey = call->function_name + "::" + std::to_string(i);
                if ((!currentClass.empty() && okContainerCallees.count(methodKey) > 0) ||
                    okContainerCallees.count(freeKey) > 0) {
                    continue;
                }
            }
            if (paramUseDisqualifiesReadOnlyContainer(call->arguments[i].get(), paramName, currentClass,
                                                       okContainerCallees)) {
                return true;
            }
        }
        return false;
    }
    // MemberAccess, NewExpression and literals never disqualify on their own.
    return false;
}

void SmartPointerAnalyzer::classifyReadOnlyContainerParams(const Program& program) {
    // Genuine-read evidence, as in classifyReadOnlyFreeFunctionParams (an unused parameter must not classify as read-only): a subscript read, `.size()`, or being the
    // container of a range-for. A bare paramName passed to an already read-only-container callee parameter counts as a read too.
    std::function<bool(const Expression*, const std::string&, const std::string&, const std::set<std::string>&)>
        exprHasGenuineReadContainer = [&](const Expression* expr, const std::string& paramName,
                                          const std::string& currentClass,
                                          const std::set<std::string>& okContainerCallees) -> bool {
        if (expr == nullptr) return false;
        if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
            if (auto id = dynamic_cast<const Identifier*>(sub->object.get()); id != nullptr &&
                id->name == paramName) {
                return true;
            }
            return exprHasGenuineReadContainer(sub->object.get(), paramName, currentClass, okContainerCallees) ||
                   exprHasGenuineReadContainer(sub->index.get(), paramName, currentClass, okContainerCallees);
        }
        if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
            if (call->callee) {
                if (auto member = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                    if (auto id = dynamic_cast<const Identifier*>(member->object.get());
                        id != nullptr && id->name == paramName && member->member == "size") {
                        return true;
                    }
                }
            }
            for (size_t i = 0; i < call->arguments.size(); ++i) {
                auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
                if (argId != nullptr && argId->name == paramName && !call->callee) {
                    const std::string methodKey =
                        currentClass + "::" + call->function_name + "::" + std::to_string(i);
                    const std::string freeKey = call->function_name + "::" + std::to_string(i);
                    if ((!currentClass.empty() && okContainerCallees.count(methodKey) > 0) ||
                        okContainerCallees.count(freeKey) > 0) {
                        return true;
                    }
                }
                if (exprHasGenuineReadContainer(call->arguments[i].get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            }
            return false;
        }
        if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
            return exprHasGenuineReadContainer(bin->left.get(), paramName, currentClass, okContainerCallees) ||
                   exprHasGenuineReadContainer(bin->right.get(), paramName, currentClass, okContainerCallees);
        }
        if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
            return exprHasGenuineReadContainer(un->operand.get(), paramName, currentClass, okContainerCallees);
        }
        return false;
    };
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyHasGenuineRead = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                  const std::string& currentClass,
                                  const std::set<std::string>& okContainerCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(ret->expression.get(), paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                if (exprHasGenuineReadContainer(decl->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(assign->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(exprStmt->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(ifs->condition.get(), paramName, currentClass, okContainerCallees)) {
                    return true;
                }
                if (bodyHasGenuineRead(ifs->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyHasGenuineRead(ifs->elseBody, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(whiles->condition.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
                if (bodyHasGenuineRead(whiles->body, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                // The container of a range-for is the genuine read itself (`for (x : paramName)`), not a disqualification.
                if (auto id = dynamic_cast<const Identifier*>(fors->container.get());
                    id != nullptr && id->name == paramName) {
                    return true;
                }
                if (exprHasGenuineReadContainer(fors->container.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
                if (bodyHasGenuineRead(fors->body, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (exprHasGenuineReadContainer(throwStmt->expression.get(), paramName, currentClass,
                                                 okContainerCallees)) {
                    return true;
                }
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyHasGenuineRead(trys->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyHasGenuineRead(trys->catchBody, paramName, currentClass, okContainerCallees)) return true;
            }
        }
        return false;
    };
    std::function<bool(const std::vector<StatementPtr>&, const std::string&, const std::string&,
                        const std::set<std::string>&)>
        bodyDisqualifiesContainer = [&](const std::vector<StatementPtr>& body, const std::string& paramName,
                                         const std::string& currentClass,
                                         const std::set<std::string>& okContainerCallees) -> bool {
        for (const auto& stmtPtr : body) {
            const Statement* stmt = stmtPtr.get();
            if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
                // Returning paramName itself hands the container out past the call, which disqualifies like any escape.
                if (paramUseDisqualifiesReadOnlyContainer(ret->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                // `Foo x = paramName;` copies or moves the whole container out, which disqualifies (`= paramName[i]` is a read and is fine).
                if (paramUseDisqualifiesReadOnlyContainer(decl->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(assign->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
                if (assign->target != nullptr) {
                    // `paramName[i] = x;` is a write and disqualifies, unlike a subscript read.
                    if (auto sub = dynamic_cast<const SubscriptExpression*>(assign->target.get())) {
                        if (auto id = dynamic_cast<const Identifier*>(sub->object.get());
                            id != nullptr && id->name == paramName) {
                            return true;
                        }
                    }
                    if (paramUseDisqualifiesReadOnlyContainer(assign->target.get(), paramName, currentClass,
                                                               okContainerCallees)) {
                        return true;
                    }
                } else if (assign->variable == paramName) {
                    // Reassigning the parameter itself (`paramName = other;`) discards the caller's container and disqualifies.
                    return true;
                }
            } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(exprStmt->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(ifs->condition.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
                if (bodyDisqualifiesContainer(ifs->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyDisqualifiesContainer(ifs->elseBody, paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(whiles->condition.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
                if (bodyDisqualifiesContainer(whiles->body, paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
                // The range-for's own container position is the genuine-read case; do not disqualify a bare paramName there.
                bool bareContainer = false;
                if (auto id = dynamic_cast<const Identifier*>(fors->container.get());
                    id != nullptr && id->name == paramName) {
                    bareContainer = true;
                }
                if (!bareContainer && paramUseDisqualifiesReadOnlyContainer(fors->container.get(), paramName,
                                                                            currentClass, okContainerCallees)) {
                    return true;
                }
                if (bodyDisqualifiesContainer(fors->body, paramName, currentClass, okContainerCallees)) return true;
            } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
                if (paramUseDisqualifiesReadOnlyContainer(throwStmt->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
                if (bodyDisqualifiesContainer(trys->body, paramName, currentClass, okContainerCallees)) return true;
                if (bodyDisqualifiesContainer(trys->catchBody, paramName, currentClass, okContainerCallees)) {
                    return true;
                }
            }
        }
        return false;
    };

    // Seed with every ext_fn `ref` container parameter (hand-declared, so not part of the fixed point), under the same "fnName::argIndex" key as an ordinary free function.
    std::set<std::string> extFnRefCallees;
    for (const auto& stmt : program.statements) {
        auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get());
        if (extFn == nullptr) continue;
        for (size_t idx : extFn->refParams) {
            if (idx < extFn->parameters.size() && isVectorType(extFn->parameters[idx].first)) {
                extFnRefCallees.insert(extFn->name + "::" + std::to_string(idx));
            }
        }
    }

    // Fixed point, same driver shape as classifyReadOnlyFreeFunctionParams.
    std::set<std::string> classified;         // "fnName::paramName"
    std::set<std::string> classifiedMethods;  // "ClassName::methodName::paramName"
    bool changed = true;
    while (changed) {
        changed = false;
        std::set<std::string> okContainerCallees = extFnRefCallees;
        for (const auto& stmt : program.statements) {
            if (auto fn2 = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (size_t i = 0; i < fn2->parameters.size(); ++i) {
                    if (classified.count(fn2->name + "::" + fn2->parameters[i].second) > 0) {
                        okContainerCallees.insert(fn2->name + "::" + std::to_string(i));
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
                            okContainerCallees.insert(cls2->name + "::" + method2->name + "::" +
                                                       std::to_string(i));
                        }
                    }
                }
            }
        }

        for (const auto& stmt : program.statements) {
            if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                for (const auto& [ptype, pname] : fn->parameters) {
                    const std::string classifiedKey = fn->name + "::" + pname;
                    if (classified.count(classifiedKey) > 0) continue;
                    // v1 scope: std::vector<primitive> only (as ext_fn `ref`).
                    if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                    if (!bodyDisqualifiesContainer(fn->body, pname, "", okContainerCallees) &&
                        bodyHasGenuineRead(fn->body, pname, "", okContainerCallees)) {
                        classified.insert(classifiedKey);
                        changed = true;
                    }
                }
            } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls->members) {
                    auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                    if (method == nullptr || !method->specializationArg.empty()) continue;
                    for (const auto& [ptype, pname] : method->parameters) {
                        const std::string classifiedKey = cls->name + "::" + method->name + "::" + pname;
                        if (classifiedMethods.count(classifiedKey) > 0) continue;
                        if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                        if (!bodyDisqualifiesContainer(method->body, pname, cls->name, okContainerCallees) &&
                            bodyHasGenuineRead(method->body, pname, cls->name, okContainerCallees)) {
                            classifiedMethods.insert(classifiedKey);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    for (const auto& key : classified) {
        readOnlyContainerParams_.insert(key);
    }
    for (const auto& key : classifiedMethods) {
        readOnlyMethodContainerParams_.insert(key);
    }
    // Also register under the qualified name, as in classifyReadOnlyFreeFunctionParams.
    for (const auto& stmt : program.statements) {
        auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get());
        if (fn == nullptr || fn->namespaceName.empty()) continue;
        for (const auto& [ptype, pname] : fn->parameters) {
            if (classified.count(fn->name + "::" + pname) > 0) {
                readOnlyContainerParams_.insert(fn->namespaceName + "::" + fn->name + "::" + pname);
            }
        }
    }
}

bool SmartPointerAnalyzer::exprReferencesName(const Expression* expr, const std::string& name) const {
    if (expr == nullptr) return false;
    if (auto id = dynamic_cast<const Identifier*>(expr)) {
        return id->name == name;
    }
    if (auto bin = dynamic_cast<const BinaryExpression*>(expr)) {
        return exprReferencesName(bin->left.get(), name) || exprReferencesName(bin->right.get(), name);
    }
    if (auto un = dynamic_cast<const UnaryExpression*>(expr)) {
        return exprReferencesName(un->operand.get(), name);
    }
    if (auto sub = dynamic_cast<const SubscriptExpression*>(expr)) {
        return exprReferencesName(sub->object.get(), name) || exprReferencesName(sub->index.get(), name);
    }
    if (auto mem = dynamic_cast<const MemberAccess*>(expr)) {
        return exprReferencesName(mem->object.get(), name);
    }
    if (auto call = dynamic_cast<const FunctionCall*>(expr)) {
        if (exprReferencesName(call->callee.get(), name)) return true;
        for (const auto& arg : call->arguments) {
            if (exprReferencesName(arg.get(), name)) return true;
        }
        return false;
    }
    if (auto neww = dynamic_cast<const NewExpression*>(expr)) {
        for (const auto& arg : neww->arguments) {
            if (exprReferencesName(arg.get(), name)) return true;
        }
        return false;
    }
    if (dynamic_cast<const ClosureExpression*>(expr) != nullptr) {
        // Any closure after the candidate assignment already disqualifies the function (bodyIsLastUseFieldAssignment gives up on closures), so the capture list is never inspected.
        return true;
    }
    // Number/StringLiteral/CharLiteral/ThisExpression/ScopedIdentifier cannot reference a bare local or parameter name.
    return false;
}

// See the header comment for the scope (straight-line bodies only, exactly one candidate assignment, no closures).
bool SmartPointerAnalyzer::bodyIsLastUseFieldAssignment(const std::vector<StatementPtr>& body,
                                                          const std::string& paramName) const {
    int candidateIndex = -1;
    for (size_t i = 0; i < body.size(); ++i) {
        const Statement* stmt = body[i].get();
        if (dynamic_cast<const IfStatement*>(stmt) != nullptr ||
            dynamic_cast<const WhileStatement*>(stmt) != nullptr ||
            dynamic_cast<const ForStatement*>(stmt) != nullptr ||
            dynamic_cast<const TryStatement*>(stmt) != nullptr) {
            return false;
        }
        if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
            // The candidate: a bare-name target (whether it is really a field is the emitter's job) other than paramName itself, with the RHS exactly the bare identifier
            // `paramName`. Any other shape still goes through the containment check below, since it may reference paramName without being the assignment we may move at.
            const bool targetIsBareNameOtherThanParam =
                assign->target == nullptr && assign->variable != paramName;
            auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
            if (targetIsBareNameOtherThanParam && rhsId != nullptr && rhsId->name == paramName) {
                if (candidateIndex != -1) return false;  // more than one -- ambiguous, give up
                candidateIndex = static_cast<int>(i);
                continue;  // the assignment's own RHS use doesn't disqualify itself
            }
            // Not the candidate shape: still check target and RHS for an ordinary reference (`x = paramName + 1;` is a use that disqualifies a later candidate).
            if (exprReferencesName(assign->target.get(), paramName) ||
                exprReferencesName(assign->expression.get(), paramName)) {
                if (candidateIndex != -1) return false;  // used again after the candidate
            }
            continue;
        }
        if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            if (exprReferencesName(decl->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            if (exprReferencesName(exprStmt->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            if (exprReferencesName(ret->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        if (auto th = dynamic_cast<const ThrowStatement*>(stmt)) {
            if (exprReferencesName(th->expression.get(), paramName) && candidateIndex != -1) return false;
            continue;
        }
        // Any other statement kind is treated as "give up", like the if/while/for/try check above.
        return false;
    }
    return candidateIndex != -1;
}

void SmartPointerAnalyzer::classifyLastUseFieldAssignedParams(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            for (const auto& [ptype, pname] : fn->parameters) {
                if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                if (bodyIsLastUseFieldAssignment(fn->body, pname)) {
                    lastUseFieldAssignedParams_.insert(fn->name + "::" + pname);
                }
            }
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                if (method == nullptr || !method->specializationArg.empty()) continue;
                for (const auto& [ptype, pname] : method->parameters) {
                    if (!isVectorType(ptype) || !isPrimitiveType(vectorElementType(ptype))) continue;
                    if (bodyIsLastUseFieldAssignment(method->body, pname)) {
                        lastUseMethodFieldAssignedParams_.insert(cls->name + "::" + method->name + "::" + pname);
                    }
                }
            }
        }
    }
}

bool SmartPointerAnalyzer::isLastUseFieldAssignedParam(const std::string& fnName,
                                                         const std::string& paramName) const {
    return lastUseFieldAssignedParams_.count(fnName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isLastUseMethodFieldAssignedParam(const std::string& className,
                                                               const std::string& methodName,
                                                               const std::string& paramName) const {
    return lastUseMethodFieldAssignedParams_.count(className + "::" + methodName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::bodyHasImpurity(const std::vector<StatementPtr>& body, const std::string& currentClass,
                                            std::set<std::string> localNames,
                                            const std::set<std::string>& pureSoFar,
                                            const std::set<std::string>& primitiveFieldNames) const {
    // Is this a local declared in the body? Mutating a body-local is fine: nothing can hand out a reference to a local.
    auto isLocal = [&](const std::string& name) { return localNames.count(name) > 0; };

    std::function<bool(const Expression*)> callIsImpure = [&](const Expression* expr) -> bool {
        auto call = dynamic_cast<const FunctionCall*>(expr);
        if (call == nullptr) return false;
        for (const auto& a : call->arguments) {
            if (callIsImpure(a.get())) return true;
        }
        if (call->callee) {
            auto member = dynamic_cast<const MemberAccess*>(call->callee.get());
            if (member != nullptr) {
                auto recvId = dynamic_cast<const Identifier*>(member->object.get());
                // A mutating container method on anything but a body-local container is an impurity: the caller can still see that container.
                if (vectorMethodInvalidatesReferences(member->member)) {
                    if (recvId == nullptr || !isLocal(recvId->name)) return true;
                    return false;
                }
                // A call on a non-local receiver (`obj.method()`, `field.method()`) is impure unless the receiver's class is known and that method is already heap-pure. A local
                // bare receiver needs no more checking.
                if (recvId != nullptr && isLocal(recvId->name)) return false;
                // A this-implicit, field or chained receiver cannot be resolved in this structural pass, so it is impure. A same-class implicit-this call is handled below.
                return true;
            }
            return true;  // unrecognized callee shape: conservatively impure
        }
        // Implicit-this method call or free-function call (callee is null either way): pure only if already classified pure this round; method-scoped key first.
        const std::string methodKey = currentClass + "::" + call->function_name;
        const std::string freeKey = call->function_name;
        if ((!currentClass.empty() && pureSoFar.count(methodKey) > 0) || pureSoFar.count(freeKey) > 0) {
            return false;
        }
        return true;
    };

    for (const auto& stmtPtr : body) {
        const Statement* stmt = stmtPtr.get();
        if (auto ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            if (callIsImpure(ret->expression.get())) return true;
        } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            if (callIsImpure(decl->expression.get())) return true;
            localNames.insert(decl->identifier);
        } else if (auto assign = dynamic_cast<const AssignmentStatement*>(stmt)) {
            if (callIsImpure(assign->expression.get())) return true;
            if (assign->target != nullptr) {
                // `x.field = ...` / `x[i] = ...` is impure unless `x` is a body-local.
                const Expression* root = assign->target.get();
                std::string rootName;
                if (auto mem = dynamic_cast<const MemberAccess*>(root)) {
                    if (auto id = dynamic_cast<const Identifier*>(mem->object.get())) rootName = id->name;
                } else if (auto sub = dynamic_cast<const SubscriptExpression*>(root)) {
                    if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) rootName = id->name;
                }
                if (rootName.empty() || !isLocal(rootName)) {
                    // `this->field = ...` has no bare-identifier root, so it is impure (assigning through `this` is never a body-local).
                    return true;
                }
            } else if (!isLocal(assign->variable) && primitiveFieldNames.count(assign->variable) == 0) {
                // Bare `name = ...` for something not declared in this body and not a primitive field (a parameter, or a non-primitive field: field writes are bare names)
                // is impure; this keeps the answer conservative.
                return true;
            }
        } else if (auto exprStmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            if (callIsImpure(exprStmt->expression.get())) return true;
        } else if (auto ifs = dynamic_cast<const IfStatement*>(stmt)) {
            if (callIsImpure(ifs->condition.get())) return true;
            if (bodyHasImpurity(ifs->body, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
            if (bodyHasImpurity(ifs->elseBody, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
        } else if (auto whiles = dynamic_cast<const WhileStatement*>(stmt)) {
            if (callIsImpure(whiles->condition.get())) return true;
            if (bodyHasImpurity(whiles->body, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
        } else if (auto fors = dynamic_cast<const ForStatement*>(stmt)) {
            if (callIsImpure(fors->container.get())) return true;
            std::set<std::string> innerLocals = localNames;
            innerLocals.insert(fors->loopVarName);
            if (bodyHasImpurity(fors->body, currentClass, innerLocals, pureSoFar, primitiveFieldNames)) return true;
        } else if (auto throwStmt = dynamic_cast<const ThrowStatement*>(stmt)) {
            if (callIsImpure(throwStmt->expression.get())) return true;
        } else if (auto trys = dynamic_cast<const TryStatement*>(stmt)) {
            if (bodyHasImpurity(trys->body, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
            if (bodyHasImpurity(trys->catchBody, currentClass, localNames, pureSoFar, primitiveFieldNames)) return true;
        }
    }
    return false;
}

void SmartPointerAnalyzer::classifyHeapPureFunctions(const Program& program) {
    // Opposite direction from the read-only fixed points: start with every function and method pure, and remove any whose own body has an impurity (checked against
    // the current set, so calling an impure one removes it too) until nothing changes. Each round must remove one, so it terminates.
    std::set<std::string> pureFree;
    std::set<std::string> pureMethods;  // "ClassName::methodName"
    // Per-class primitive field names (a field write is a bare name; see bodyHasImpurity).
    std::map<std::string, std::set<std::string>> primitiveFieldsByClass;
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            pureFree.insert(fn->name);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            auto& primitiveFields = primitiveFieldsByClass[cls->name];
            for (const auto& member : cls->members) {
                if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    pureMethods.insert(cls->name + "::" + method->name);
                } else if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    if (isPrimitiveType(field->type)) primitiveFields.insert(field->identifier);
                }
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        std::set<std::string> pureSoFar = pureFree;
        for (const auto& m : pureMethods) pureSoFar.insert(m);

        for (const auto& stmt : program.statements) {
            if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
                if (pureFree.count(fn->name) == 0) continue;  // already removed
                std::set<std::string> localNames;
                for (const auto& [ptype, pname] : fn->parameters) localNames.insert(pname);
                if (bodyHasImpurity(fn->body, "", localNames, pureSoFar, {})) {
                    pureFree.erase(fn->name);
                    changed = true;
                }
            } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
                for (const auto& member : cls->members) {
                    auto method = dynamic_cast<const MethodDeclaration*>(member.get());
                    if (method == nullptr) continue;
                    const std::string key = cls->name + "::" + method->name;
                    if (pureMethods.count(key) == 0) continue;
                    std::set<std::string> localNames;
                    for (const auto& [ptype, pname] : method->parameters) localNames.insert(pname);
                    const auto& primitiveFields = primitiveFieldsByClass[cls->name];
                    if (bodyHasImpurity(method->body, cls->name, localNames, pureSoFar, primitiveFields)) {
                        pureMethods.erase(key);
                        changed = true;
                    }
                }
            }
        }
    }

    heapPureFreeFunctions_ = std::move(pureFree);
    // heapPureMethods_ uses the same combined "Class::method" keys as isHeapPureMethod, so store verbatim.
    heapPureMethods_ = std::move(pureMethods);
}


// ---- read-only string parameters --------------------------------------------------------------------
//
// A `string` parameter is read-only when the body never mutates it: no `p.append/push_back/clear/reserve(...)`, no `p = ...`, no `p[i] = ...`, and it is not
// handed to an ext_fn `ref string`. Copying it onward is a read. Such a parameter is lowered to `const std::string&`.
//
// The hazard is aliasing: for a field argument the callee might mutate that very field while holding the reference. So the emitter passes an explicit copy
// for a field argument unless the callee provably mutates no field of that name. This pass computes, per function and method, the field names it
// transitively mutates (name-based and conservative; "*" means it may mutate anything, e.g. an unresolved call).
namespace {
bool isStringMutatorMethod(const std::string& m) {
    return m == "append" || m == "push_back" || m == "clear" || m == "reserve" || m == "resize" ||
           m == "insert" || m == "erase" || m == "pop_back" || m == "assign" || m == "replace" ||
           m == "copyWithin";
}
}  // namespace

void SmartPointerAnalyzer::classifyReadOnlyStringParams(const Program& program) {
    struct Fn {
        std::string key;                  // "name" for a free function, "Class::name" for a method
        std::string bareName;
        std::string className;            // "" for a free function
        const std::vector<std::pair<std::string, std::string>>* params = nullptr;
        const std::vector<StatementPtr>* body = nullptr;
        bool skipParams = false;          // virtual / override / in an inheritance hierarchy
    };
    std::vector<Fn> fns;
    std::set<std::string> freeNames;
    std::map<std::string, std::set<std::string>> methodsByName;  // bare method name -> class keys
    std::set<std::string> classesInHierarchy;
    for (const auto& stmt : program.statements) {
        if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            if (!cls->baseClass.empty()) {
                classesInHierarchy.insert(cls->name);
                classesInHierarchy.insert(cls->baseClass);
            }
        }
    }
    std::map<std::string, std::set<std::string>> fieldNamesByClass;
    objectFieldNames_.clear();
    for (const auto& stmt : program.statements) {
        if (auto fn = dynamic_cast<const FunctionDeclaration*>(stmt.get())) {
            Fn f;
            f.key = fn->name;
            f.bareName = fn->name;
            f.params = &fn->parameters;
            f.body = &fn->body;
            fns.push_back(f);
            freeNames.insert(fn->name);
        } else if (auto cls = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            for (const auto& member : cls->members) {
                if (auto field = dynamic_cast<const VariableDeclaration*>(member.get())) {
                    fieldNamesByClass[cls->name].insert(field->identifier);
                    // a field that owns objects (a class handle or a container): writing it can free them
                    if (!isPrimitiveType(field->type)) objectFieldNames_.insert(field->identifier);
                } else if (auto method = dynamic_cast<const MethodDeclaration*>(member.get())) {
                    if (!method->specializationArg.empty() || !method->typeParameter.empty()) continue;
                    Fn f;
                    f.key = cls->name + "::" + method->name;
                    f.bareName = method->name;
                    f.className = cls->name;
                    f.params = &method->parameters;
                    f.body = &method->body;
                    f.skipParams = method->isVirtual || method->isOverride ||
                                   classesInHierarchy.count(cls->name) > 0;
                    fns.push_back(f);
                    methodsByName[method->name].insert(f.key);
                } else if (auto ctor = dynamic_cast<const ConstructorDeclaration*>(member.get())) {
                    Fn f;
                    f.key = cls->name + "::" + ctor->name;
                    f.bareName = ctor->name;
                    f.className = cls->name;
                    f.params = &ctor->parameters;
                    f.body = &ctor->body;
                    f.skipParams = true;  // constructors keep by-value string parameters
                    fns.push_back(f);
                    methodsByName["<ctor>" + cls->name].insert(f.key);
                }
            }
        }
    }
    // ext_fn `ref string` parameters are mutable references
    std::map<std::string, std::set<size_t>> extFnMutableStringArgs;
    for (const auto& stmt : program.statements) {
        auto extFn = dynamic_cast<const ExternFunctionDeclaration*>(stmt.get());
        if (extFn == nullptr) continue;
        for (size_t idx : extFn->refParams) {
            if (idx < extFn->parameters.size() && isStringType(extFn->parameters[idx].first)) {
                extFnMutableStringArgs[extFn->name].insert(idx);
            }
        }
    }

    struct Facts {
        std::set<std::string> mutatedParams;
        std::set<std::string> mutatedFields;  // may contain "*"
        std::set<std::string> calls;          // keys of user functions/methods (transitive closure below)
    };
    std::map<std::string, Facts> facts;

    // one walk per function
    for (const auto& f : fns) {
        Facts& fa = facts[f.key];
        std::set<std::string> locals;
        for (const auto& [ptype, pname] : *f.params) locals.insert(pname);
        std::function<void(const std::vector<StatementPtr>&)> collectLocals =
            [&](const std::vector<StatementPtr>& stmts) {
                for (const auto& sp : stmts) {
                    if (auto d = dynamic_cast<const VariableDeclaration*>(sp.get())) locals.insert(d->identifier);
                    else if (auto c = dynamic_cast<const ContainerDeclaration*>(sp.get())) locals.insert(c->identifier);
                    else if (auto i = dynamic_cast<const IfStatement*>(sp.get())) { collectLocals(i->body); collectLocals(i->elseBody); }
                    else if (auto w = dynamic_cast<const WhileStatement*>(sp.get())) collectLocals(w->body);
                    else if (auto fo = dynamic_cast<const ForStatement*>(sp.get())) { locals.insert(fo->loopVarName); collectLocals(fo->body); }
                    else if (auto t = dynamic_cast<const TryStatement*>(sp.get())) { collectLocals(t->body); collectLocals(t->catchBody); }
                }
            };
        collectLocals(*f.body);

        // name of the field a mutation lands on, or "" when the receiver is a local
        auto fieldOfReceiver = [&](const Expression* recv) -> std::string {
            if (auto id = dynamic_cast<const Identifier*>(recv)) {
                return locals.count(id->name) > 0 ? std::string() : id->name;
            }
            if (auto m = dynamic_cast<const MemberAccess*>(recv)) return m->member;
            if (auto sub = dynamic_cast<const SubscriptExpression*>(recv)) {
                if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) {
                    return locals.count(id->name) > 0 ? std::string() : id->name;
                }
                if (auto m = dynamic_cast<const MemberAccess*>(sub->object.get())) return m->member;
            }
            return std::string();
        };
        auto noteMutation = [&](const Expression* recv) {
            if (auto id = dynamic_cast<const Identifier*>(recv)) {
                if (locals.count(id->name) > 0) { fa.mutatedParams.insert(id->name); return; }
            }
            if (auto sub = dynamic_cast<const SubscriptExpression*>(recv)) {
                if (auto id = dynamic_cast<const Identifier*>(sub->object.get()); id != nullptr && locals.count(id->name) > 0) {
                    fa.mutatedParams.insert(id->name);
                    return;
                }
            }
            const std::string field = fieldOfReceiver(recv);
            if (!field.empty()) fa.mutatedFields.insert(field);
            else fa.mutatedFields.insert("*");
        };

        std::function<void(const Expression*)> visitExpr = [&](const Expression* e) {
            if (e == nullptr) return;
            if (auto b = dynamic_cast<const BinaryExpression*>(e)) { visitExpr(b->left.get()); visitExpr(b->right.get()); return; }
            if (auto u = dynamic_cast<const UnaryExpression*>(e)) { visitExpr(u->operand.get()); return; }
            if (auto m = dynamic_cast<const MemberAccess*>(e)) { visitExpr(m->object.get()); return; }
            if (auto s = dynamic_cast<const SubscriptExpression*>(e)) { visitExpr(s->object.get()); visitExpr(s->index.get()); return; }
            if (auto n = dynamic_cast<const NewExpression*>(e)) { for (const auto& a : n->arguments) visitExpr(a.get()); return; }
            if (auto call = dynamic_cast<const FunctionCall*>(e)) {
                for (const auto& a : call->arguments) visitExpr(a.get());
                if (call->callee) {
                    visitExpr(call->callee.get());
                    if (auto m = dynamic_cast<const MemberAccess*>(call->callee.get())) {
                        if (isStringMutatorMethod(m->member)) noteMutation(m->object.get());
                        // a call to a user method of that name (receiver class not resolved here: every class that has a method of this name is a candidate)
                        auto it = methodsByName.find(m->member);
                        if (it != methodsByName.end()) {
                            for (const auto& k : it->second) fa.calls.insert(k);
                        }
                    }
                } else {
                    const std::string& name = call->function_name;
                    if (freeNames.count(name) > 0) fa.calls.insert(name);
                    // implicit-this call to a method of the current class
                    if (!f.className.empty()) {
                        auto it = methodsByName.find(name);
                        if (it != methodsByName.end()) for (const auto& k : it->second) fa.calls.insert(k);
                    }
                    // a constructor-style or unknown name resolves to nothing we can see: no effect on fields
                    if (auto ext = extFnMutableStringArgs.find(name); ext != extFnMutableStringArgs.end()) {
                        for (size_t idx : ext->second) {
                            if (idx < call->arguments.size()) noteMutation(call->arguments[idx].get());
                        }
                    }
                }
                return;
            }
        };
        std::function<void(const std::vector<StatementPtr>&)> visitStmts = [&](const std::vector<StatementPtr>& stmts) {
            for (const auto& sp : stmts) {
                const Statement* st = sp.get();
                if (auto d = dynamic_cast<const VariableDeclaration*>(st)) visitExpr(d->expression.get());
                else if (auto a = dynamic_cast<const AssignmentStatement*>(st)) {
                    visitExpr(a->expression.get());
                    if (a->target) {
                        visitExpr(a->target.get());
                        noteMutation(a->target.get());
                    } else if (locals.count(a->variable) > 0) {
                        fa.mutatedParams.insert(a->variable);
                        // reassigning an object-typed parameter drops the handle the caller passed in, which can free the object a field argument lives in
                        for (const auto& [ptype2, pname2] : *f.params) {
                            if (pname2 == a->variable && !isPrimitiveType(ptype2)) fa.mutatedFields.insert("*");
                        }
                    } else {
                        fa.mutatedFields.insert(a->variable);
                    }
                }
                else if (auto es = dynamic_cast<const ExpressionStatement*>(st)) visitExpr(es->expression.get());
                else if (auto r = dynamic_cast<const ReturnStatement*>(st)) visitExpr(r->expression.get());
                else if (auto th = dynamic_cast<const ThrowStatement*>(st)) visitExpr(th->expression.get());
                else if (auto i = dynamic_cast<const IfStatement*>(st)) { visitExpr(i->condition.get()); visitStmts(i->body); visitStmts(i->elseBody); }
                else if (auto w = dynamic_cast<const WhileStatement*>(st)) { visitExpr(w->condition.get()); visitStmts(w->body); }
                else if (auto fo = dynamic_cast<const ForStatement*>(st)) { visitExpr(fo->container.get()); visitStmts(fo->body); }
                else if (auto t = dynamic_cast<const TryStatement*>(st)) { visitStmts(t->body); visitStmts(t->catchBody); }
            }
        };
        visitStmts(*f.body);
    }

    // transitive closure of the mutated-field sets over the call graph
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [key, fa] : facts) {
            for (const auto& callee : std::set<std::string>(fa.calls)) {
                auto it = facts.find(callee);
                if (it == facts.end()) continue;
                for (const auto& fld : it->second.mutatedFields) {
                    if (fa.mutatedFields.insert(fld).second) changed = true;
                }
            }
        }
    }

    readOnlyStringParams_.clear();
    readOnlyStringPositions_.clear();
    stringMutatedFieldsByFunction_.clear();
    for (const auto& f : fns) {
        const Facts& fa = facts[f.key];
        // keyed by the bare callee name (free function or method): the emitter does not always know the receiver's class, so a call site unions every function/method of that name
        auto& set = stringMutatedFieldsByFunction_[f.bareName];
        set.insert(fa.mutatedFields.begin(), fa.mutatedFields.end());
        if (f.skipParams) continue;
        for (const auto& [ptype, pname] : *f.params) {
            if (!isStringType(ptype)) continue;
            if (fa.mutatedParams.count(pname) > 0) continue;
            readOnlyStringParams_.insert(f.key + "::" + pname);
            for (size_t pi = 0; pi < f.params->size(); ++pi) {
                if ((*f.params)[pi].second == pname) readOnlyStringPositions_.insert(f.bareName + "#" + std::to_string(pi));
            }
        }
    }
}

bool SmartPointerAnalyzer::isReadOnlyStringParam(const std::string& className, const std::string& functionName,
                                                 const std::string& paramName) const {
    const std::string key = className.empty() ? functionName : className + "::" + functionName;
    return readOnlyStringParams_.count(key + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::anyReadOnlyStringParamAt(const std::string& bareName, size_t index) const {
    return readOnlyStringPositions_.count(bareName + "#" + std::to_string(index)) > 0;
}

bool SmartPointerAnalyzer::calleeMayMutateObjects(const std::string& bareName) const {
    auto it = stringMutatedFieldsByFunction_.find(bareName);
    if (it == stringMutatedFieldsByFunction_.end()) return true;
    if (it->second.count("*") > 0) return true;
    for (const auto& fld : it->second) {
        if (objectFieldNames_.count(fld) > 0) return true;
    }
    return false;
}

bool SmartPointerAnalyzer::calleeMayMutateField(const std::string& bareName, const std::string& fieldName) const {
    auto it = stringMutatedFieldsByFunction_.find(bareName);
    if (it == stringMutatedFieldsByFunction_.end()) return true;  // unknown callee: assume it may
    return it->second.count("*") > 0 || it->second.count(fieldName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyFreeFunctionParam(const std::string& fnName,
                                                        const std::string& paramName) const {
    return readOnlyFreeFunctionParams_.count(fnName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyMethodParam(const std::string& className, const std::string& methodName,
                                                  const std::string& paramName) const {
    return readOnlyMethodParams_.count(className + "::" + methodName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyContainerParam(const std::string& fnName,
                                                     const std::string& paramName) const {
    return readOnlyContainerParams_.count(fnName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isReadOnlyMethodContainerParam(const std::string& className,
                                                           const std::string& methodName,
                                                           const std::string& paramName) const {
    return readOnlyMethodContainerParams_.count(className + "::" + methodName + "::" + paramName) > 0;
}

bool SmartPointerAnalyzer::isHeapPureFreeFunction(const std::string& fnName) const {
    return heapPureFreeFunctions_.count(fnName) > 0;
}

bool SmartPointerAnalyzer::isHeapPureMethod(const std::string& className, const std::string& methodName) const {
    return heapPureMethods_.count(className + "::" + methodName) > 0;
}

void SmartPointerAnalyzer::firstPassCollectReferences(const Program& program) {
    for (const auto& stmt : program.statements) {
        if (auto classDecl = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames.insert(classDecl->name);
            classRelationships[classDecl->name].className = classDecl->name;
        }
    }

    // Classes first, base before derived, so analyzeClassDeclaration can re-register an inherited field under the derived scope. Functions and main come afterward; they only reference classes.
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
    // Give each object-typed parameter a bare variableReferences entry so getPointerType(className, paramName) finds something instead of falling back to shared.
    // Deliberately minimal: only registerReference + registerSymbolType. A parameter never stored into a field defaults to unique, like a fresh local; one that is
    // stored (`next = child;`) is promoted to shared by scanSource's regex pass.
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
    // Also register the qualified name (`YAML::Node`): registration is gated on `classNames.find(type)` with the literal declared-type string, so a namespaced local
    // would never be registered at all. classRelationships stays keyed by the simple name.
    if (!cls.namespaceName.empty()) {
        classNames.insert(cls.namespaceName + "::" + cls.name);
    }

    // Inherited fields need entries under the derived class's scope too, or getPointerType falls through to its shared default. This re-runs the base's member
    // registration in the derived scope; base-before-derived order comes from firstPassCollectReferences's caller.
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
                    ref.aliasedWithField = true;
                    ref.assignedFromVars.insert(rhs.memberName);
                    for (auto& [otherKey, otherRef] : variableReferences) {
                        if (otherRef.variableName == rhs.memberName) {
                            otherRef.isAssignedTo = true;
                            otherRef.aliasedWithField = true;
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
                            ref.aliasedWithField = true;
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
                        if (rhs.isMemberAccess) ref.aliasedWithField = true;
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
                        ref.aliasedWithField = true;
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
        if (classNames.find(ref.containingScope) == classNames.end()) continue;  // fields only
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

    std::set<std::string> fieldMemberKeys;
    std::set<std::string> fieldNames;
    for (const auto& [fkey, fref] : variableReferences) {
        if (classNames.find(fref.containingScope) == classNames.end()) continue;
        fieldMemberKeys.insert(makeMemberKey(fref.className, fref.variableName));
        fieldMemberKeys.insert(makeMemberKey(fref.containingScope, fref.variableName));
        fieldNames.insert(fref.variableName);
    }
    for (auto& [key, ref] : variableReferences) {
        PointerType pointerType = PointerType::UNIQUE;
        std::string reason = "default unique ownership";

        const std::string exactMemberKey = makeMemberKey(ref.className, ref.variableName);
        // A hint is written `//@ptr <kind> EnclosingClass.member`, but ref.className is the field's type, not its enclosing class, so look the enclosing class up too.
        const std::string enclosingMemberKey = makeMemberKey(ref.containingScope, ref.variableName);
        auto explicitIt = explicitHints.find(exactMemberKey);
        if (explicitIt == explicitHints.end()) explicitIt = explicitHints.find(enclosingMemberKey);
        const auto fallbackHintIt = explicitHints.find(ref.variableName);
        // Hints and back-reference rules are about fields (containing scope is a class). A parameter or local that shares a field's name (`void link(Item prev, ...)`)
        // is an ordinary variable and must not become a weak_ptr.
        const bool isField = classNames.find(ref.containingScope) != classNames.end();
        // A hint names a field whenever a field of that name exists; it reaches a local or parameter only when it does not (`//@ptr shared Node.a` on a local `a`).
        const bool hintReachesThis = isField ||
            (fieldMemberKeys.count(exactMemberKey) == 0 && fieldMemberKeys.count(enclosingMemberKey) == 0 &&
             fieldNames.count(ref.variableName) == 0);

        if (hintReachesThis && explicitIt != explicitHints.end()) {
            pointerType = explicitIt->second;
            ref.hasExplicitHint = true;
            reason = "explicit @ptr annotation";
        } else if (hintReachesThis && fallbackHintIt != explicitHints.end()) {
            pointerType = fallbackHintIt->second;
            ref.hasExplicitHint = true;
            reason = "explicit @ptr annotation";
        } else if (isField && (ref.isBackReference || bidirectionalBackMembers.find(exactMemberKey) != bidirectionalBackMembers.end())) {
            pointerType = PointerType::WEAK;
            reason = ref.participatesInBidirectionalAssignment
                ? "bidirectional assignment closed a cycle"
                : "back-reference heuristic matched a backward edge";
            classesNeedingSharedRoots.insert(ref.className);
            classRelationships[ref.className].hasBackReference = true;
        } else if (isField && (ref.participatesInBidirectionalAssignment ||
                   bidirectionalOwnerMembers.find(exactMemberKey) != bidirectionalOwnerMembers.end())) {
            pointerType = PointerType::SHARED;
            reason = "bidirectional ownership needs a shared forward edge";
            classesNeedingSharedRoots.insert(ref.className);
        } else if (ref.aliasedWithField) {
            pointerType = PointerType::SHARED;
            reason = "copied between a field and another variable, so both are owners";
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
    // Does `target` (a `Class.member`, `Scope.member` or bare name) name a field of some class?
    auto hintNamesAField = [&](const std::string& target) {
        for (const auto& [fkey, fref] : variableReferences) {
            if (classNames.find(fref.containingScope) == classNames.end()) continue;
            if (target == fref.variableName || target == makeMemberKey(fref.className, fref.variableName) ||
                target == makeMemberKey(fref.containingScope, fref.variableName)) {
                return true;
            }
        }
        return false;
    };
    for (const auto& [target, line] : explicitHintLines) {
        bool matched = false;
        for (auto& [key, ref] : variableReferences) {
            const std::string memberKey = makeMemberKey(ref.className, ref.variableName);
            const std::string enclosingKey = makeMemberKey(ref.containingScope, ref.variableName);
            // A hint names a field. A parameter or local that shares the field's name must not inherit it: fields are the entries whose containing scope is a class.
            if (classNames.find(ref.containingScope) == classNames.end() && hintNamesAField(target)) continue;
            if (target == ref.variableName || target == memberKey || target == enclosingKey) {
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
