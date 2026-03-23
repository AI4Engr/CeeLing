#include "smart_pointer_analyzer.h"
#include <iostream>
#include <algorithm>
#include <queue>

namespace cling {

namespace {
constexpr const char* kGlobalScope = "__global__";
}

void SmartPointerAnalyzer::analyzeProgram(const Program& program) {
    // Clear previous analysis
    variableReferences.clear();
    classRelationships.clear();
    pointerDecisions.clear();
    classNames.clear();
    
    // Two-pass analysis
    firstPassCollectReferences(program);
    secondPassDeterminePointerTypes();
}

PointerType SmartPointerAnalyzer::getPointerType(const std::string& className, const std::string& variableName) const {
    const std::string qualifiedSuffix = "|" + className + "." + variableName;
    bool found = false;
    PointerType resolved = PointerType::SHARED;

    for (const auto& [key, decision] : pointerDecisions) {
        if (key.size() >= qualifiedSuffix.size() &&
            key.compare(key.size() - qualifiedSuffix.size(), qualifiedSuffix.size(), qualifiedSuffix) == 0) {
            if (!found) {
                resolved = decision;
                found = true;
            } else if (resolved != decision) {
                // Conflicting scoped decisions should prefer conservative shared ownership.
                return PointerType::SHARED;
            }
        }
    }

    if (found) {
        return resolved;
    }

    // Default to shared_ptr if not analyzed (safer option)
    return PointerType::SHARED;
}

void SmartPointerAnalyzer::firstPassCollectReferences(const Program& program) {
    std::cerr << "=== FIRST PASS: Collecting Reference Relationships ===" << std::endl;
    
    // Collect all class names first
    for (const auto& stmt : program.statements) {
        if (auto classDecl = dynamic_cast<const ClassDeclaration*>(stmt.get())) {
            classNames.insert(classDecl->name);
            ClassRelationship relationship;
            relationship.className = classDecl->name;
            classRelationships[classDecl->name] = std::move(relationship);
        }
    }
    
    // Analyze all statements to collect relationships
    for (const auto& stmt : program.statements) {
        analyzeStatement(*stmt);
    }
    
    // Debug output
    std::cerr << "Found " << classNames.size() << " classes: ";
    for (const auto& name : classNames) {
        std::cerr << name << " ";
    }
    std::cerr << std::endl;
    
    std::cerr << "Found " << variableReferences.size() << " class variable references" << std::endl;
}

void SmartPointerAnalyzer::analyzeStatement(const Statement& stmt, const std::string& currentClass) {
    if (auto classDecl = dynamic_cast<const ClassDeclaration*>(&stmt)) {
        analyzeClassDeclaration(*classDecl);
    } else if (auto varDecl = dynamic_cast<const VariableDeclaration*>(&stmt)) {
        analyzeVariableDeclaration(*varDecl, currentClass);
    } else if (auto assignment = dynamic_cast<const AssignmentStatement*>(&stmt)) {
        analyzeAssignmentStatement(*assignment, currentClass);
    } else if (auto funcDecl = dynamic_cast<const FunctionDeclaration*>(&stmt)) {
        // Analyze function body
        for (const auto& bodyStmt : funcDecl->body) {
            analyzeStatement(*bodyStmt, currentClass);
        }
    } else if (auto mainFunc = dynamic_cast<const MainFunction*>(&stmt)) {
        // Analyze main function body
        for (const auto& bodyStmt : mainFunc->body) {
            analyzeStatement(*bodyStmt, "main");
        }
    } else if (auto ctorDecl = dynamic_cast<const ConstructorDeclaration*>(&stmt)) {
        // Analyze constructor body
        for (const auto& bodyStmt : ctorDecl->body) {
            analyzeStatement(*bodyStmt, currentClass);
        }
    } else if (auto methodDecl = dynamic_cast<const MethodDeclaration*>(&stmt)) {
        // Analyze method body
        for (const auto& bodyStmt : methodDecl->body) {
            analyzeStatement(*bodyStmt, currentClass);
        }
    }
}

void SmartPointerAnalyzer::analyzeVariableDeclaration(const VariableDeclaration& decl, const std::string& currentClass) {
    // Check if this is a class type variable
    if (classNames.find(decl.type) != classNames.end()) {
        const std::string normalizedScope = normalizeScope(currentClass);
        std::string key = makeKey(normalizedScope, decl.type, decl.identifier);
        
        VariableReference ref;
        ref.className = decl.type;
        ref.variableName = decl.identifier;
        ref.containingClass = normalizedScope;
        
        // Check if it's initialized with another variable or new expression
        if (decl.expression) {
            if (auto identifier = dynamic_cast<const Identifier*>(decl.expression.get())) {
                // Assignment from another variable: Node temp = head;
                ref.isAssignedFrom = true;
                ref.assignedFromVars.insert(identifier->name);
                recordAssignment(identifier->name, decl.identifier, currentClass);
            } else if (auto newExpr = dynamic_cast<const NewExpression*>(decl.expression.get())) {
                // Assignment from new: Node newNode = new Node(value);
                // This is a creation, not a sharing
            }
        }
        
        variableReferences[key] = ref;
        
        // Record class relationship
        if (!currentClass.empty() && classRelationships.find(currentClass) != classRelationships.end()) {
            classRelationships[currentClass].memberVariables.insert(decl.identifier);
            classRelationships[currentClass].references.insert(decl.type);
            classRelationships[decl.type].referencedBy.insert(currentClass);
        }
        
        std::cerr << "Found class variable: " << decl.type << " " << decl.identifier 
                  << " in " << (currentClass.empty() ? "global" : currentClass) << std::endl;
    }
}

void SmartPointerAnalyzer::analyzeAssignmentStatement(const AssignmentStatement& stmt, const std::string& currentClass) {
    // Analyze the expression to see what's being assigned
    if (auto identifier = dynamic_cast<const Identifier*>(stmt.expression.get())) {
        // Assignment from another variable: temp = temp.next;
        recordAssignment(identifier->name, stmt.variable, currentClass);
    } else if (auto memberAccess = dynamic_cast<const MemberAccess*>(stmt.expression.get())) {
        // Assignment from member access: temp = temp.next;
        if (auto objIdentifier = dynamic_cast<const Identifier*>(memberAccess->object.get())) {
            std::string sourceVar = objIdentifier->name + "." + memberAccess->member;
            recordAssignment(sourceVar, stmt.variable, currentClass);

            // If a member is used as a source in assignments, it is likely shared/read by multiple owners.
            for (auto& [key, ref] : variableReferences) {
                if (ref.variableName == memberAccess->member) {
                    ref.isAssignedTo = true;
                    ref.assignedToVars.insert(stmt.variable);
                }
            }
        }
    } else if (auto newExpr = dynamic_cast<const NewExpression*>(stmt.expression.get())) {
        // Assignment from new: left = new Node(value);
        // This creates a new object, not sharing
    }
}

void SmartPointerAnalyzer::analyzeClassDeclaration(const ClassDeclaration& cls) {
    // Analyze class members
    for (const auto& member : cls.members) {
        analyzeStatement(*member, cls.name);
    }
}

void SmartPointerAnalyzer::secondPassDeterminePointerTypes() {
    std::cerr << "\n=== SECOND PASS: Determining Pointer Types ===" << std::endl;

    // First, detect back-references in classes with multiple same-type members
    detectBackReferences();

    for (const auto& [key, ref] : variableReferences) {
        PointerType pointerType = PointerType::UNIQUE; // Default to unique_ptr (Rule A)

        // Rule C: Back-reference in bidirectional structure (e.g., prev in doubly linked list)
        if (ref.isBackReference) {
            pointerType = PointerType::WEAK;
            std::cerr << "BACK-REFERENCE: " << key << " -> weak_ptr (Rule C)" << std::endl;
        }
        // Rule B: Check for shared ownership (multiple variables point to same object)
        else if (isSharedOwnership(ref.containingClass, ref.className, ref.variableName)) {
            pointerType = PointerType::SHARED;
            std::cerr << "SHARED OWNERSHIP: " << key << " -> shared_ptr (Rule B)" << std::endl;
        }
        // Rule A: Parent-child / single ownership (default for class members)
        else {
            pointerType = PointerType::UNIQUE;
            std::cerr << "SINGLE OWNERSHIP: " << key << " -> unique_ptr (Rule A)" << std::endl;
        }

        pointerDecisions[key] = pointerType;
    }
}

void SmartPointerAnalyzer::detectBackReferences() {
    // For each class, find members that are back-references
    // A back-reference is detected when:
    // 1. A class has multiple members of the same type (e.g., next and prev both of type Node)
    // 2. The member name suggests backward direction (prev, parent, back)

    std::map<std::string, std::vector<std::string>> classMembersByType;

    // Group variables by containing class and type
    for (auto& [key, ref] : variableReferences) {
        if (ref.containingClass != kGlobalScope && ref.containingClass != "main") {
            std::string groupKey = ref.containingClass + ":" + ref.className;
            classMembersByType[groupKey].push_back(ref.variableName);
        }
    }

    // Check for back-references
    for (const auto& [groupKey, members] : classMembersByType) {
        if (members.size() >= 2) {
            // Multiple members of same type - check for back-reference patterns
            for (const auto& memberName : members) {
                // Common back-reference names
                if (memberName == "prev" || memberName == "previous" ||
                    memberName == "parent" || memberName == "back" ||
                    memberName == "owner") {
                    // Find and mark this variable as back-reference
                    for (auto& [key, ref] : variableReferences) {
                        if (ref.variableName == memberName &&
                            groupKey == ref.containingClass + ":" + ref.className) {
                            ref.isBackReference = true;
                            std::cerr << "Detected back-reference: " << ref.containingClass
                                      << "." << memberName << std::endl;
                        }
                    }
                }
            }
        }
    }
}

bool SmartPointerAnalyzer::isParentChildRelationship(const std::string& containingClass, const std::string& className, const std::string& variableName) const {
    std::string key = makeKey(containingClass, className, variableName);
    auto it = variableReferences.find(key);
    if (it == variableReferences.end()) return true; // Default assumption
    
    const auto& ref = it->second;
    
    // If this variable is only assigned to (never assigned from others), 
    // it's likely a parent-child relationship
    return !ref.isAssignedFrom && ref.assignedFromVars.empty();
}

bool SmartPointerAnalyzer::isSharedOwnership(const std::string& containingClass, const std::string& className, const std::string& variableName) const {
    std::string key = makeKey(containingClass, className, variableName);
    auto it = variableReferences.find(key);
    if (it == variableReferences.end()) return false;
    
    const auto& ref = it->second;
    
    // Multiple variables are assigned from this one, or this is assigned from multiple sources
    return (ref.assignedToVars.size() > 1) || (ref.assignedFromVars.size() > 1) || 
           (ref.isAssignedFrom && ref.isAssignedTo);
}

bool SmartPointerAnalyzer::isPartOfCycle(const std::string& containingClass, const std::string& className, const std::string& variableName) const {
    // Check if the class this variable references has a back-reference to the containing class
    std::string key = makeKey(containingClass, className, variableName);
    auto it = variableReferences.find(key);
    if (it == variableReferences.end()) return false;
    
    const auto& ref = it->second;
    
    // Simple cycle detection: if class A references class B, and class B references class A
    return detectCycleBetweenClasses(ref.containingClass, className);
}

bool SmartPointerAnalyzer::detectCycleBetweenClasses(const std::string& class1, const std::string& class2) const {
    if (class1.empty() || class2.empty() || class1 == class2) return false;
    
    auto it1 = classRelationships.find(class1);
    auto it2 = classRelationships.find(class2);
    
    if (it1 == classRelationships.end() || it2 == classRelationships.end()) return false;
    
    // Direct cycle: class1 references class2 AND class2 references class1
    bool class1ToClass2 = it1->second.references.find(class2) != it1->second.references.end();
    bool class2ToClass1 = it2->second.references.find(class1) != it2->second.references.end();
    
    if (class1ToClass2 && class2ToClass1) {
        std::cerr << "Direct cycle detected between " << class1 << " and " << class2 << std::endl;
        return true;
    }
    
    // Enhanced cycle detection using DFS to find indirect cycles
    std::set<std::string> visited;
    std::set<std::string> recursionStack;
    
    return hasCycleDFS(class1, class2, visited, recursionStack);
}

bool SmartPointerAnalyzer::hasCycleDFS(const std::string& current, const std::string& target, 
                                      std::set<std::string>& visited, std::set<std::string>& recursionStack) const {
    if (current == target && !recursionStack.empty()) {
        return true; // Found cycle back to target
    }
    
    if (visited.find(current) != visited.end()) {
        return false; // Already visited this node
    }
    
    visited.insert(current);
    recursionStack.insert(current);
    
    auto it = classRelationships.find(current);
    if (it != classRelationships.end()) {
        for (const std::string& referenced : it->second.references) {
            if (recursionStack.find(referenced) != recursionStack.end()) {
                // Found a cycle in recursion stack
                if (referenced == target) {
                    std::cerr << "Indirect cycle detected: " << current << " -> " << referenced << std::endl;
                    return true;
                }
            } else if (hasCycleDFS(referenced, target, visited, recursionStack)) {
                return true;
            }
        }
    }
    
    recursionStack.erase(current);
    return false;
}

std::string SmartPointerAnalyzer::normalizeScope(const std::string& containingClass) const {
    return containingClass.empty() ? kGlobalScope : containingClass;
}

std::string SmartPointerAnalyzer::makeKey(const std::string& containingClass, const std::string& className, const std::string& variableName) const {
    return normalizeScope(containingClass) + "|" + className + "." + variableName;
}

void SmartPointerAnalyzer::recordAssignment(const std::string& fromVar, const std::string& toVar, const std::string& currentClass) {
    const std::string normalizedScope = normalizeScope(currentClass);

    // Find variable references in the current scope.
    for (auto& [key, ref] : variableReferences) {
        if (ref.variableName == toVar && ref.containingClass == normalizedScope) {
            ref.isAssignedFrom = true;
            ref.assignedFromVars.insert(fromVar);
        }
        
        if (ref.variableName == fromVar && ref.containingClass == normalizedScope) {
            ref.isAssignedTo = true;
            ref.assignedToVars.insert(toVar);
        }
    }
    
    std::cerr << "Recorded assignment: " << fromVar << " -> " << toVar 
              << " in scope " << normalizedScope << std::endl;
}

} // namespace cling
