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

struct AnalysisDiagnostic {
    int line = 0;
    std::string message;
    std::string suggestion;
};

struct VariableReference {
    std::string className;        // Type of this variable
    std::string variableName;
    std::string containingScope;  // Which class/function contains this variable
    int line = 0;
    bool isAssignedFrom = false;  // Is this variable assigned from another variable?
    bool isAssignedTo = false;    // Is another variable assigned from this?
    bool isBackReference = false; // Is this a backward reference (e.g., prev in doubly linked list)?
    bool participatesInBidirectionalAssignment = false;
    bool hasExplicitHint = false;
    std::set<std::string> assignedFromVars;  // Variables assigned to this one
    std::set<std::string> assignedToVars;    // Variables this one is assigned to
    std::string reason;
};

struct ClassRelationship {
    std::string className;
    std::set<std::string> memberVariables;     // Member variables of this class type
    std::set<std::string> referencedBy;       // Classes that reference this class
    std::set<std::string> references;         // Classes this class references
    bool hasBackReference = false;           // Potential cycle indicator
    bool hasBidirectionalAssignment = false;
};

class SmartPointerAnalyzer {
public:
    // Two-pass analysis
    void analyzeProgram(const Program& program, const std::string& source = "");
    PointerType getPointerType(const std::string& className, const std::string& variableName) const;
    PointerType getPointerTypeForVariable(const std::string& variableName) const;
    std::string buildAnalysisReport() const;
    const std::vector<AnalysisDiagnostic>& getDiagnostics() const;
    bool classRequiresSharedRoot(const std::string& className) const;

private:
    // First pass: collect all reference relationships
    void firstPassCollectReferences(const Program& program);
    void analyzeStatement(const Statement& stmt, const std::string& currentClass = "");
    void analyzeVariableDeclaration(const VariableDeclaration& decl, const std::string& currentClass);
    void analyzeAssignmentStatement(const AssignmentStatement& stmt, const std::string& currentClass);
    void analyzeClassDeclaration(const ClassDeclaration& cls);

    void scanSource(const std::string& source);
    void parseAnnotationLine(const std::string& trimmedLine, int lineNumber);
    void registerReference(const std::string& scopeName, const std::string& className,
                           const std::string& variableName, int lineNumber);
    void registerSymbolType(const std::string& scopeName, const std::string& symbolName,
                            const std::string& className);
    void recordBidirectionalAssignment(const std::string& lhsObject, const std::string& lhsMember,
                                       const std::string& rhsObject, int lineNumber,
                                       const std::string& scopeName);

    // Second pass: determine pointer types based on ownership patterns
    void secondPassDeterminePointerTypes();
    void detectBackReferences();  // Detect backward references in bidirectional structures
    bool isSharedOwnership(const std::string& containingScope, const std::string& className, const std::string& variableName) const;
    void applyExplicitHints();
    void addDiagnostic(int line, const std::string& message, const std::string& suggestion = "");
    std::string buildKey(const std::string& containingScope, const std::string& className,
                         const std::string& variableName) const;
    std::string normalizeScope(const std::string& containingScope) const;

    // Data structures for analysis
    std::map<std::string, VariableReference> variableReferences;  // Key: containingClass|className.variableName
    std::map<std::string, ClassRelationship> classRelationships;
    std::map<std::string, PointerType> pointerDecisions;          // Key: containingClass|className.variableName
    std::map<std::string, std::string> pointerReasons;
    std::set<std::string> classNames;
    std::map<std::string, PointerType> explicitHints;
    std::map<std::string, int> explicitHintLines;
    std::set<std::string> classesNeedingSharedRoots;
    std::set<std::string> bidirectionalOwnerMembers;
    std::set<std::string> bidirectionalBackMembers;
    std::map<std::string, std::string> symbolTypesByScope;
    std::map<std::string, std::pair<std::string, int>> bidirectionalAssignmentSeeds;
    std::vector<AnalysisDiagnostic> diagnostics;
    std::vector<std::string> reportLines;
};

} // namespace cling
