#include "smart_pointer_analyzer.h"
#include "ast_walk.h"

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
    // What this file's OWN code reaches in other files' classes: `x.member` (a field, or a container field) and `x.method(...)`. A kind is demanded only for
    // something that is reached. The analysis also promotes things by NAME alone (every field `gate` of class FileGate becomes shared because one `gate` is handed
    // on), which says nothing about a field this file never touches; demanding it would change a class that is fine as it was.
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
                        // (the reason is a comment after the three words: it says WHY this file needs the member shared)
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

    // what the files that include a class of this file need from it (setDemands): the same plumbing as a written //@ptr hint, and a return kind
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

    // Independent of everything else below -- reads only the raw Program, writes
    // only valueTypeClasses_/readOnlyFreeFunctionParams_/readOnlyMethodParams_/
    // readOnlyContainerParams_/readOnlyMethodContainerParams_/
    // heapPureFreeFunctions_/heapPureMethods_/lastUseFieldAssignedParams_/
    // lastUseMethodFieldAssignedParams_ -- so they can run first with no
    // ordering dependency between each other. classifyReadOnlyContainerParams
    // does not depend on classifyHeapPureFunctions's result (they answer
    // different questions -- "is this param never mutated through" vs. "can
    // this function invalidate a caller's OWN container/object"), so order
    // between the two does not matter. classifyLastUseFieldAssignedParams is
    // likewise independent -- a pure per-function/method scan with no fixed
    // point and no dependency on any other classification's result.
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
    // Runs BEFORE secondPassDeterminePointerTypes: a field this pass decides
    // needs SHARED (because some method returns it as an alias) writes into
    // explicitHints, which secondPass's applyExplicitHints call then picks up
    // through the exact same lookup path a real //@ptr hint would use --
    // ownership is INFERRED here, never user-annotated (CLAUDE.md), but reuses
    // the hint mechanism's plumbing rather than adding a parallel one.
    inferAliasReturningMethods(program);
    secondPassDeterminePointerTypes();
    // `b = a;` followed by another use of `a` means two live handles to one object -- the documented
    // "multiple live owners => shared_ptr" rule. Without this the assignment moves `a` and every
    // later use is rejected, which makes ordinary Java-style aliasing (a `head` and a `current`
    // walking the same list) unwritable.
    forceVirtualSignaturesShared(program);
    promoteAliasedLocals(program);

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
    // pointerDecisions keys are buildKey(containingScope, fieldTYPE, name) = "Scope|Type.name".
    // For a field, the scope is the enclosing class; the type is whatever the field is declared
    // as, which the caller does not need to know -- match on scope prefix and ".name" suffix.
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
        // only a container of OBJECTS that this program declares (containers are told apart by name alone, and another file may have a vector of strings by the same name)
        auto type = containerFieldTypes_.find(name);
        if (type != containerFieldTypes_.end() && isVectorType(type->second) && classNames.count(vectorElementType(type->second)) > 0) {
            containerElementShared_[name] = true;
        }
    }
    // A value pushed into a container whose elements are shared must itself be shared -- decided only
    // once every push and every element copy has been seen. (A parameter that is pushed then makes its
    // callers pass a shared object, via the call-site promotion that runs after this.)
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
        const Identifier* recvId = asIdentifier(member->object.get());
        const Identifier* arg = asIdentifier(call->arguments[0].get());
        // the container is a variable (`v.push_back(x)`) or a field reached through an object (`obj.items.push_back(x)`,
        // keyed by the field's name like every other container here)
        const MemberAccess* recvField = dynamic_cast<const MemberAccess*>(member->object.get());
        if ((recvId == nullptr && recvField == nullptr) || arg == nullptr) return;
        const std::string recvName = recvId != nullptr ? recvId->name : recvField->member;

        // Resolve the argument's OWN already-decided pointer kind via its symbol
        // type (registered the same way locals/parameters/fields already are).
        // Precise scope first -- see scanContainerElementKindsInStatement's
        // header comment for why the kGlobalScope fallback alone collides
        // across unrelated functions/methods sharing a local's bare name.
        auto typeIt = symbolTypesByScope.find(makeSymbolKey(scopeName, arg->name));
        if (typeIt == symbolTypesByScope.end() && scanLocals_.count(arg->name) == 0) {
            typeIt = symbolTypesByScope.find(makeSymbolKey(kGlobalScope, arg->name));
        }
        if (typeIt == symbolTypesByScope.end()) return;
        // Only an OBJECT argument says anything about the container's element kind. getPointerType's not-found
        // fallback is SHARED, so a string / int / vector variable (`parts.push_back(current)`) used to make a
        // vector<string> look like a vector of shared objects and emit `std::shared_ptr<>(current)`.
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
        // `Cmd c = items[i];` -- an owning local initialised from a container element: two owners of one
        // object, so the elements must be shared (a unique_ptr element cannot be copied out).
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
        // An element copied from an element, `v[i] = v[j];` (the swap-with-last removal), or into a variable, `x = v[i];`: two owners of one object, which only
        // shared elements can be. The container is known to hold objects from its DECLARED type (`std::vector<Cmd>`), not from a push_back: a class in a file
        // of its own may never push into its own vector (the callers do), and must still come out right when it is analysed alone.
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
        } else if (auto trys = dynamic_cast<const TryStatement*>(stmtPtr.get())) {
            collectReturnExpressions(trys->body, out);
            collectReturnExpressions(trys->catchBody, out);
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
    // Iterated to a fixed point: a function that forwards `return other(...)` is classified from
    // `other`, which may be defined later in the file or classified by an earlier pass
    // (promoteReturnsOfSharedLocals marks a free function that returns a shared local).
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
                // The returned field must itself be a shared handle: resolve `base.field` (one
                // level, or a chain) to the field's owning class and mark that field shared.
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
    // `Item w = get(i); ... return w;` -- a local initialised from a call whose result is shared IS shared (AstEmitter::isSharedReturningCall declares it
    // so), but the table of local decisions the pass below reads does not know that, so such a method stayed unique_ptr-returning while its local was a
    // shared_ptr: a compile error in the generated code. Which methods return shared depends on this pass itself, so it runs to a fixed point.
    for (bool changed = true; changed;) {
        changed = false;
        promoteReturnsOfSharedLocalsOnce(program, changed);
    }
}

void SmartPointerAnalyzer::promoteReturnsOfSharedLocalsOnce(const Program& program, bool& changed) {
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
            if (getPointerTypeForVariableInScope(fn->name, id->name) == PointerType::SHARED ||
                localComesFromSharedCall(findLocalInitializer(fn->body, id->name), "", fn->name)) {
                anyLocalIsShared = true;
                break;
            }
        }
        if (anyLocalIsShared) {
            changed = true;
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

// One entry per statement in execution (pre-)order. `used` holds the names read by the statement's
// OWN expressions (a loop's entry holds its condition / container); nested bodies get entries of
// their own.
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
        // `T x = v[i];` / `T x = obj.items[i];` copies a handle out of a container element: x and the element
        // are two owners.
        if (auto sub = dynamic_cast<const SubscriptExpression*>(rhs);
            sub != nullptr && types.find(target) != types.end() &&
            (dynamic_cast<const Identifier*>(sub->object.get()) != nullptr ||
             dynamic_cast<const MemberAccess*>(sub->object.get()) != nullptr)) {
            promote(target);
            continue;
        }
        // `T x = field;` inside a method: the local and the field are two owners from this point on, whether or
        // not the field is touched again (a field copy never moves the field). Both sides become shared;
        // otherwise the local is a shared_ptr initialised from a unique_ptr field.
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
        // inside a loop, a use anywhere in the loop is 'after' on the next iteration, unless the
        // source is declared inside that loop (then every iteration starts with a fresh one)
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
        // A spawn job's constructor MOVES its object captures into unique fields; the usual rule ("a parameter
        // stored into a field is linked, so both become shared") would make the creating thread keep a live
        // owner of an object that now runs concurrently. DanglingChecker::checkSpawnCall decides what may be moved.
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
    // Later extension (cJSON port): the same promotion applies to a free function, to a store nested
    // inside if/while/for/try, and to a store through another object (`array.child = item;`). A
    // promotion is always safe -- SHARED only means the caller's variable is copied instead of
    // moved -- so the original reason for staying out of branches (not promising sharing on a path
    // that never stores) costs nothing and is dropped. The field's owning class is resolved from the
    // target's base expression (parameter, local, or field chain); when that fails the enclosing
    // class is used, as before.
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

        // RHS must be one of this body's own parameters, or (when the field is already shared) a
        // local declared in it -- a local stored into a shared field must itself be shared, or the
        // emitted `field = local` assigns a unique_ptr lvalue to a shared_ptr.
        const bool rhsIsParam = std::any_of(params.begin(), params.end(),
            [&](const auto& p) { return p.second == rhsId->name; });
        const bool rhsIsLocal = !rhsIsParam && localTypes.count(rhsId->name) > 0;
        if (!rhsIsParam && !rhsIsLocal) continue;

        // The field's own declared type (registered by analyzeVariableDeclaration
        // when the class body was first walked) tells us which (class, name) pair
        // to ask getPointerType about.
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
            //
            // `<<` is ALSO integer shift now (one shared grammar tier -- see
            // grammar.js), so this no longer sees stream chains exclusively. No
            // stream-vs-shift test is needed to stay correct, though: reaching
            // the promotion below requires the program to declare a free
            // `operator<<` whose SECOND parameter is a known CLASS type, and an
            // Identifier right operand whose own declared type matches it. A
            // shift's operands are integers, so it fails `classNames.find` and
            // falls straight through. The "left is always the stream" remark
            // above is therefore still true of every case that gets PAST these
            // guards, rather than of every `<<` node reaching this branch.
            // AstEmitter::isStreamChain is where the distinction is actually
            // made, because there it changes the emitted output.
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
        // `paramName == x` / `paramName != x` compares identity (a null check, or two handles to
        // the same node); it reads the smart pointer and neither consumes nor stores it, exactly
        // like `!paramName` below. Only a BARE paramName operand is exempted; anything nested in
        // the other operand is still checked.
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

bool SmartPointerAnalyzer::paramUseDisqualifiesReadOnlyContainer(
    const Expression* expr, const std::string& paramName, const std::string& currentClass,
    const std::set<std::string>& okContainerCallees) const {
    if (expr == nullptr) return false;
    // A bare occurrence of paramName disqualifies UNLESS the immediate
    // context (subscript read, .size() receiver, range-for container, or an
    // already-read-only callee argument) recognizes and clears it below --
    // this function is only ever called on positions that are NOT one of
    // those recognized contexts (see the callers: assignment RHS/target,
    // return, call arguments other than the recognized bare-to-read-only-
    // callee case, binary/unary operands), matching how
    // paramUseDisqualifiesReadOnly treats a bare Identifier as always
    // disqualifying in the positions IT is called from.
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
        // A subscript READ (`paramName[i]` used as a value) never
        // disqualifies -- the caller only descends into a subscript's INDEX
        // expression here (see bodyDisqualifiesContainer's AssignmentStatement
        // target handling, which checks a subscript TARGET separately and
        // never reaches this branch for `paramName[i] = x`). Still recurse
        // into a nested object (`m[paramName[i]]`) and the index itself.
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
                    // `.size()` is a genuine read, never disqualifying.
                    // Every other method on a container in CeeLing's
                    // allow-list (isKnownVectorMethod) either mutates
                    // (push_back/pop_back/resize -- disqualify unconditionally,
                    // even called on a body-local copy of paramName's contents
                    // makes no sense since paramName IS the receiver here) or
                    // is begin()/end() (an iterator can outlive a reallocation
                    // -- see CLAUDE.md threat #4 -- so treated conservatively
                    // as disqualifying too, not modeled as a form of read).
                    // `reserve` falls in here too (not modeled as a read) --
                    // conservative, not a safety requirement (it changes
                    // neither size() nor any element), just not yet worth a
                    // dedicated case since nothing calls reserve() on a
                    // parameter it also wants classified read-only.
                    if (member->member == "size") return false;
                    return true;
                }
                // A nested receiver (`paramName.field.method()`) is a
                // MemberAccess object -- not descended into here, matching
                // paramUseDisqualifiesReadOnly's own MemberAccess handling:
                // reading a field through paramName never disqualifies.
            }
        }
        for (size_t i = 0; i < call->arguments.size(); ++i) {
            auto argId = dynamic_cast<const Identifier*>(call->arguments[i].get());
            if (argId != nullptr && argId->name == paramName && !call->callee) {
                // Bare paramName passed to a free function/method's own
                // parameter at this position -- OK if that target parameter
                // is itself already known read-only-container (fixed point,
                // same key shapes as classifyReadOnlyContainerParams builds)
                // OR an ext_fn `ref` parameter (externRefParams_ is
                // DanglingChecker's, not this analyzer's -- ext_fn calls are
                // always free functions with no receiver, recognized here by
                // name never colliding with a CeeLing-authored function,
                // matched via the same "fnName::argIndex" key
                // classifyReadOnlyContainerParams uses for ordinary free
                // functions, since an ext_fn's `refParams` are registered
                // into that SAME free-function parameter list at parse time
                // -- see ExternFunctionDeclaration in ast.h).
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
    // MemberAccess (paramName.field -- not a container-typed field access
    // question this pass handles), NewExpression, literals: never
    // disqualifying on their own.
    return false;
}

void SmartPointerAnalyzer::classifyReadOnlyContainerParams(const Program& program) {
    // Genuine-read evidence, mirroring classifyReadOnlyFreeFunctionParams's
    // own positive-evidence requirement (see that function's header comment
    // for why an unused parameter must NOT classify as read-only): a
    // subscript READ, `.size()`, or being the container of a range-for.
    // `okContainerCallees`/`currentClass` are threaded through so a bare
    // paramName passed to an ALREADY-classified-read-only-container callee
    // parameter counts as a genuine read here too, not merely a
    // non-disqualifying use -- mirrors the object-typed rule's own
    // FunctionCall case in classifyReadOnlyFreeFunctionParams (needed for
    // the identical reason: `sumTwice(data) { return sumAll(data) + 0; }`
    // never touches `data.field`/`data[i]` itself, only sumAll does, so
    // without this sumTwice would fail the genuine-read requirement forever
    // despite being exactly as read-only as sumAll).
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
                // The container of a range-for IS the genuine read itself
                // (`for (x : paramName)`) -- a bare-identifier container,
                // unlike every other position, is the evidence, not a
                // disqualification (checked by NOT calling
                // paramUseDisqualifiesReadOnlyContainer on fors->container
                // in bodyDisqualifiesContainer below either).
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
                // Returning paramName itself (bare identifier) hands the
                // container out past the call -- disqualifies exactly like
                // any other escape. paramUseDisqualifiesReadOnlyContainer's
                // bare-Identifier base case already covers this.
                if (paramUseDisqualifiesReadOnlyContainer(ret->expression.get(), paramName, currentClass,
                                                           okContainerCallees)) {
                    return true;
                }
            } else if (auto decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
                // `Foo x = paramName;` (or `= paramName[i]`, which is fine --
                // paramUseDisqualifiesReadOnlyContainer's subscript-read
                // handling only recognizes paramName as an OBJECT/INDEX
                // position, and a bare paramName RHS still hits the
                // bare-Identifier base case) copies/moves the whole
                // container out -- disqualifies.
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
                    // `paramName[i] = x;` (SubscriptExpression target) is a
                    // WRITE -- disqualifies even though a subscript READ does
                    // not. Checked directly here (not via
                    // paramUseDisqualifiesReadOnlyContainer, whose subscript
                    // handling is read-shaped) since this is the one position
                    // a subscript's meaning flips.
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
                    // Reassigning the parameter itself (`paramName = other;`,
                    // no ->target, bare-name LHS) discards the caller's
                    // container entirely -- disqualifies. (A plain read-only
                    // container param is never itself the LHS in any
                    // legitimate read-only use.)
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
                // The range-for's OWN container position is the genuine-read
                // case (see bodyHasGenuineRead) -- do not disqualify a bare
                // paramName container here, only a more complex expression's
                // sub-parts (e.g. `for (x : paramName.field)`, unaffected
                // either way since MemberAccess never disqualifies).
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

    // Seed with every ext_fn's `ref` container parameters -- static facts,
    // not part of the fixed point (an ext_fn has no body to classify FROM,
    // its ref-ness is hand-declared). Registered under the plain
    // "fnName::argIndex" key, the same shape an ordinary free function's
    // entry uses, since an ext_fn IS a free function as far as a caller's
    // bare-argument-passed-onward check is concerned.
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

    // Fixed point, mirroring classifyReadOnlyFreeFunctionParams's own driver
    // shape exactly (see its header comment for the termination argument).
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
                    // v1 scope: std::vector<primitive> only (matches ext_fn
                    // `ref`'s own container restriction -- no ownership/
                    // aliasing ambiguity a primitive element could introduce).
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
    // Dual-register under the qualified name too, same reasoning as
    // classifyReadOnlyFreeFunctionParams's own trailing block.
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
        // A closure captures by NAME (see ClosureExpression::captures), not
        // by reference -- but bodyIsLastUseFieldAssignment treats a closure
        // ANYWHERE as an unconditional "give up" (see its own doc comment),
        // so this never needs to inspect the capture list: any
        // ClosureExpression reachable from a statement after the candidate
        // assignment already disqualifies the whole function, whether or
        // not it happens to capture `name`.
        return true;
    }
    // Number/StringLiteral/CharLiteral/ThisExpression/ScopedIdentifier: none
    // can reference a bare local/parameter name.
    return false;
}

// See the header comment for the exact scope (straight-line bodies only,
// exactly one candidate assignment, no closures anywhere).
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
            // The candidate shape: a bare-name target (`fieldName = ...;`,
            // target == null -- see AssignmentStatement's own comment;
            // whether that name is actually a FIELD, as opposed to a plain
            // local variable, is deliberately NOT checked here -- see this
            // function's own header comment for why that's the emitter's
            // job, not this classification's) whose target name differs
            // from paramName itself (excludes the degenerate `bytes =
            // bytes;`) and whose RHS is exactly the bare identifier
            // `paramName`. Any other shape (a member-access target like
            // `this->x = paramName;`/`a.b = paramName;`, or any RHS other
            // than a bare identifier) is NOT a candidate -- it still needs
            // the containment check below, same as any other statement,
            // since it may well reference paramName without being the one
            // assignment we're allowed to move at.
            const bool targetIsBareNameOtherThanParam =
                assign->target == nullptr && assign->variable != paramName;
            auto rhsId = dynamic_cast<const Identifier*>(assign->expression.get());
            if (targetIsBareNameOtherThanParam && rhsId != nullptr && rhsId->name == paramName) {
                if (candidateIndex != -1) return false;  // more than one -- ambiguous, give up
                candidateIndex = static_cast<int>(i);
                continue;  // the assignment's own RHS use doesn't disqualify itself
            }
            // Not the candidate shape -- still check both target and RHS
            // for an ordinary reference (e.g. `x = paramName + 1;` uses
            // paramName without being movable, and must still disqualify a
            // LATER candidate the same as any other use).
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
        // Any other statement kind this v1 doesn't specifically recognize
        // (ContainerDeclaration, nested class/function declarations, ...)
        // is conservatively treated as an unconditional "give up" -- matching
        // the same discipline the if/while/for/try check above already
        // applies structurally.
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
    // Does a bare identifier name a LOCAL this body itself declared (via
    // VariableDeclaration)? Mutating a body-local is fine -- it cannot alias
    // anything the caller can see, since CeeLing has no way to hand out a
    // reference to a local (no `ref` params, closures capture by value).
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
                // A mutating container method (push_back/pop_back/resize) on
                // anything but a body-local container is an impurity: it can
                // reallocate/destroy elements of a container the CALLER can
                // still see (its own local, a field reached through `this`,
                // or a container reached through a parameter).
                if (vectorMethodInvalidatesReferences(member->member)) {
                    if (recvId == nullptr || !isLocal(recvId->name)) return true;
                    return false;
                }
                // A call on a receiver that is itself a non-local object
                // (`obj.method()`, `field.method()`) -- conservatively
                // impure unless the receiver's class is known and that
                // SPECIFIC method is already classified heap-pure. A bare
                // receiver Identifier that IS local recurses no further (its
                // own body was already checked when IT was classified).
                if (recvId != nullptr && isLocal(recvId->name)) return false;
                // this-implicit or field/chained receiver: resolve via
                // classFields_ is out of scope for a purely structural,
                // Program-only pass (no type resolution here) -- treat
                // conservatively as impure. This only affects the RECEIVER
                // case (`x.foo()`); a same-class implicit-this call
                // (`foo()`, call->callee null) is handled below instead.
                return true;
            }
            return true;  // unrecognized callee shape: conservatively impure
        }
        // Implicit-this method call or free-function call
        // (`foo(...)`/`this.foo(...)` -- call->callee is null either way,
        // matching every other pass in this file's own established
        // disambiguation comment). Pure only if already classified pure this
        // round (fixed point) -- method-scoped key tried first.
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
                // `x.field = ...` / `x[i] = ...` -- impure unless `x` is a
                // body-local (see isLocal's own reasoning: a local's storage
                // is never visible to the caller).
                const Expression* root = assign->target.get();
                std::string rootName;
                if (auto mem = dynamic_cast<const MemberAccess*>(root)) {
                    if (auto id = dynamic_cast<const Identifier*>(mem->object.get())) rootName = id->name;
                } else if (auto sub = dynamic_cast<const SubscriptExpression*>(root)) {
                    if (auto id = dynamic_cast<const Identifier*>(sub->object.get())) rootName = id->name;
                }
                if (rootName.empty() || !isLocal(rootName)) {
                    // `this->field = ...` has no bare-identifier root (member
                    // object is a ThisExpression, not an Identifier) --
                    // rootName stays empty, correctly falling into "impure"
                    // (assigning through `this` is never a body-local).
                    return true;
                }
            } else if (!isLocal(assign->variable) && primitiveFieldNames.count(assign->variable) == 0) {
                // Bare `name = ...` reassigning something NOT declared in
                // this body and NOT a known-primitive field -- a parameter,
                // or (CeeLing field writes ARE bare names, e.g. `bitBuffer =
                // bitBuffer >> n;`, not `this->bitBuffer = ...`) a
                // non-primitive field. Assigning a NEW object/container to
                // replace what a parameter used to point at cannot itself
                // dangle anything (CeeLing's move semantics mean the
                // caller's own local is already consumed/moved-from by this
                // point if the parameter were object-typed) -- but it also
                // means this parameter no longer aliases what the caller
                // passed in for the REST of the body, which is exactly the
                // "not heap-pure" question this pass exists to answer
                // conservatively. Treated as impure to stay conservative;
                // not a case any of the motivating call sites (deflate/
                // inflate accumulator fields) actually need to pass.
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
    // Opposite monotonic direction from the read-only fixed points above:
    // starts by assuming every free function/method is pure, then removes
    // any whose OWN body has an impurity (checked against the current
    // pureSoFar set, so a function calling an impure one loses purity too)
    // until nothing changes -- same termination argument (bounded by the
    // number of functions+methods, each round must remove at least one to
    // continue).
    std::set<std::string> pureFree;
    std::set<std::string> pureMethods;  // "ClassName::methodName"
    // Per-class primitive field names -- see bodyHasImpurity's own doc
    // comment for why a bare-name reassignment needs this (CeeLing field
    // writes are bare names, not `this->field = ...`).
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
    // Split the combined "ClassName::methodName" keys back into
    // heapPureMethods_'s own storage (kept in the same combined-key shape
    // isHeapPureMethod's query already builds, so no split is actually
    // needed -- store verbatim).
    heapPureMethods_ = std::move(pureMethods);
}


// ---- read-only string parameters ---------------------------------------------------------------------
//
// CeeLing has no reference parameters, so every `string` argument used to be copied on every call -- the
// whole string, regardless of whether the callee ever changes it. A `string` parameter is READ-ONLY when the
// body never mutates it: no `p.append/push_back/clear/reserve(...)`, no `p = ...`, no `p[i] = ...`, and it is
// not handed to an ext_fn `ref string` (a mutable reference). Copying it onward (`x = p`, `f(p)`, `return p`,
// `out.append(p)`) is just a read. Such a parameter is lowered to `const std::string&`.
//
// The one real hazard is aliasing. For a LOCAL or by-value parameter argument nothing the callee does can
// reach the caller's string, but for a FIELD argument the callee might mutate that very field while it holds
// a reference to it (or, without the reference, would have been looking at a snapshot). So the emitter passes
// an explicit copy for a field argument unless the callee provably mutates no field of that name: this pass
// also computes, per function and method, the set of field names it (transitively) mutates -- a name-based,
// conservative set, with "*" meaning "may mutate anything" (a call it could not resolve to code it can see).
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
                        // a call to a user method of that name (receiver class not resolved here: every class
                        // that has a method of this name is a candidate)
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
                        // reassigning an object-typed PARAMETER drops the handle the caller passed in, which can
                        // free the object a field argument lives in
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
        // keyed by the BARE callee name (free function or method): the emitter does not always know the
        // receiver's class, so a call site unions every function/method of that name
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
        // A hint is written `//@ptr <kind> EnclosingClass.member`, but ref.className is the FIELD's
        // type, not its enclosing class: for `Node prev;` inside Node the two coincide, which is
        // why only that shape ever matched. For `Emitter owner;` inside Listener the key built
        // above is `Emitter.owner` and a hint for `Listener.owner` could never be found (every
        // cross-class field silently fell back to the default). Look the enclosing class up too.
        const std::string enclosingMemberKey = makeMemberKey(ref.containingScope, ref.variableName);
        auto explicitIt = explicitHints.find(exactMemberKey);
        if (explicitIt == explicitHints.end()) explicitIt = explicitHints.find(enclosingMemberKey);
        const auto fallbackHintIt = explicitHints.find(ref.variableName);
        // Hints and back-reference rules are about FIELDS (containing scope is a class). A parameter
        // or local that merely shares a field's name -- `void link(Item prev, ...)` -- is an ordinary
        // variable and must not become a weak_ptr because of it.
        const bool isField = classNames.find(ref.containingScope) != classNames.end();
        // A hint names a field whenever a field of that name exists; it reaches a local or parameter
        // only when it does not (`//@ptr shared Node.a` on a local `a`).
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
    // Does `target` (a `Class.member`, `Scope.member` or bare name) name a FIELD of some class?
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
            // A hint names a FIELD. A parameter or local that happens to share the field's name
            // (`void link(Item prev, ...)` next to a `//@ptr weak Item.prev`) must not inherit it:
            // fields are the entries whose containing scope is a class.
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
