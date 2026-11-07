#include "passes/smart_pointer_analyzer.h"
#include "passes/smart_pointer_util.h"
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
