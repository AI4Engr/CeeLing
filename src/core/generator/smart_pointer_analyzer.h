#pragma once

#include "../cst_to_ast/ast.h"
#include <string>
#include <map>
#include <set>
#include <vector>

namespace cling {

enum class PointerType {
    UNIQUE,   // unique_ptr - exclusive ownership (Rule A)
    SHARED,   // shared_ptr - shared ownership (Rule B)
    WEAK      // weak_ptr - cycle breaking (Rule C)
};

struct VariableReference {
    std::string className;        // Type of this variable
    std::string variableName;
    std::string containingClass;  // Which class contains this variable
    bool isAssignedFrom = false;  // Is this variable assigned from another variable?
    bool isAssignedTo = false;    // Is another variable assigned from this?
    bool isBackReference = false; // Is this a backward reference (e.g., prev in doubly linked list)?
    std::set<std::string> assignedFromVars;  // Variables assigned to this one
    std::set<std::string> assignedToVars;    // Variables this one is assigned to
};

struct ClassRelationship {
    std::string className;
    std::set<std::string> memberVariables;     // Member variables of this class type
    std::set<std::string> referencedBy;       // Classes that reference this class
    std::set<std::string> references;         // Classes this class references
    bool hasBackReference = false;           // Potential cycle indicator
};

class SmartPointerAnalyzer {
public:
    // Two-pass analysis
    void analyzeProgram(const Program& program);
    PointerType getPointerType(const std::string& className, const std::string& variableName) const;
    
private:
    // First pass: collect all reference relationships
    void firstPassCollectReferences(const Program& program);
    void analyzeStatement(const Statement& stmt, const std::string& currentClass = "");
    void analyzeExpression(const Expression& expr, const std::string& currentClass = "");
    void analyzeVariableDeclaration(const VariableDeclaration& decl, const std::string& currentClass);
    void analyzeAssignmentStatement(const AssignmentStatement& stmt, const std::string& currentClass);
    void analyzeClassDeclaration(const ClassDeclaration& cls);
    
    // Second pass: determine pointer types based on ownership patterns
    void secondPassDeterminePointerTypes();
    void detectBackReferences();  // Detect backward references in bidirectional structures
    bool isParentChildRelationship(const std::string& containingClass, const std::string& className, const std::string& variableName) const;
    bool isSharedOwnership(const std::string& containingClass, const std::string& className, const std::string& variableName) const;
    bool isPartOfCycle(const std::string& containingClass, const std::string& className, const std::string& variableName) const;
    bool detectCycleBetweenClasses(const std::string& class1, const std::string& class2) const;
    bool hasCycleDFS(const std::string& current, const std::string& target, 
                     std::set<std::string>& visited, std::set<std::string>& recursionStack) const;
    
    // Data structures for analysis
    std::map<std::string, VariableReference> variableReferences;  // Key: containingClass|className.variableName
    std::map<std::string, ClassRelationship> classRelationships;
    std::map<std::string, PointerType> pointerDecisions;          // Key: containingClass|className.variableName
    std::set<std::string> classNames;
    
    // Helper functions
    std::string normalizeScope(const std::string& containingClass) const;
    std::string makeKey(const std::string& containingClass, const std::string& className, const std::string& variableName) const;
    void recordAssignment(const std::string& fromVar, const std::string& toVar, const std::string& currentClass);
};

} // namespace cling
