#pragma once

#include "syntax/ast.h"
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
    // Copied out of or into a field: the field and the other side are two owners of one object, so both are shared.
    bool aliasedWithField = false;
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

// Chooses unique_ptr / shared_ptr / weak_ptr for every object variable, field, container element and method result,
// and classifies parameters (read-only, last-use) for the emitter.
class SmartPointerAnalyzer {
public:
    void analyzeProgram(const Program& program, const std::string& source = "");

    // Pointer demands. Pointer kinds are chosen per file, but a file that includes a class may need a kind the
    // declaring file did not choose (see --ptr-demands in the guide). exportDemands writes what this file needs;
    // setDemands reads them back. A demand can only make a member shared. Line forms: "shared field Class.name",
    // "shared elements name", "shared return Class::method".
    void setDemands(const std::vector<std::string>& lines);
    std::vector<std::string> exportDemands(const Program& program) const;
    PointerType getPointerType(const std::string& className, const std::string& variableName) const;
    // The decision recorded for this (scope, class, name), else the merged getPointerType answer, which
    // is SHARED when same-named variables disagree. scopeName is a free function's name, "main" or "Class::method".
    PointerType getPointerTypeInScope(const std::string& scopeName, const std::string& className,
                                      const std::string& variableName) const;
    PointerType getPointerTypeForVariable(const std::string& variableName) const;
    // Like getPointerTypeForVariable but only matches decisions recorded under scopeName, so a same-named local
    // in another function cannot collide.
    PointerType getPointerTypeForVariableInScope(const std::string& scopeName,
                                                  const std::string& variableName) const;
    std::string buildAnalysisReport() const;
    const std::vector<AnalysisDiagnostic>& getDiagnostics() const;
    bool classRequiresSharedRoot(const std::string& className) const;

    // Is this field of this class lowered to a weak_ptr? Exact lookup. A weak_ptr cannot be read directly,
    // so the emitter reads such a field through lock().
    bool isWeakField(const std::string& enclosingClass, const std::string& fieldName) const;

    // Is the element kind of this container SHARED (true) or UNIQUE (false)? One kind per container
    // variable, because vector<unique_ptr<T>> and vector<shared_ptr<T>> are different types. Never WEAK.
    bool containerElementIsShared(const std::string& containerVarName) const;

    // A method's return pointer kind. SHARED when every `return` hands back `this`, an object field or another
    // alias-returning method (see inferAliasReturningMethods); otherwise UNIQUE.
    PointerType getMethodReturnPointerType(const std::string& className,
                                           const std::string& methodName) const;

    // Does the class need enable_shared_from_this: some alias-returning method contains `return this;`.
    bool classNeedsSharedFromThis(const std::string& className) const;

    // Is the class a value type (never wrapped in a smart pointer)? True iff it defines operator*, operator++
    // and operator!= (the range-for iterator protocol). Independent of PointerType; the emitter checks it first.
    bool isValueTypeClass(const std::string& className) const;

    // Is a free function's object parameter read-only: only ever read as `param.field`, never stored,
    // returned or moved on? Such a parameter is not consumed by the call (see classifyReadOnlyFreeFunctionParams).
    bool isReadOnlyFreeFunctionParam(const std::string& fnName, const std::string& paramName) const;

    // The same for a method's parameter, keyed by (class, method, param).
    bool isReadOnlyMethodParam(const std::string& className, const std::string& methodName,
                                const std::string& paramName) const;

    // Is a free function's std::vector<primitive> parameter read-only, so it can be passed as const&?
    // Stricter than the object rule: a mutating method call (push_back, pop_back, resize) disqualifies
    // (see classifyReadOnlyContainerParams).
    bool isReadOnlyContainerParam(const std::string& fnName, const std::string& paramName) const;

    // The same for a method's container parameter.
    bool isReadOnlyMethodContainerParam(const std::string& className, const std::string& methodName,
                                          const std::string& paramName) const;

    // A string parameter the body never mutates (see classifyReadOnlyStringParams): lowered to
    // `const std::string&`. className is "" for a free function.
    bool isReadOnlyStringParam(const std::string& className, const std::string& functionName,
                               const std::string& paramName) const;
    // May the function/method named `bareName` (transitively) mutate a field called `fieldName`?
    // True for an unknown callee. Decides whether a FIELD argument to a read-only string parameter
    // can be passed by reference or needs an explicit copy.
    bool calleeMayMutateField(const std::string& bareName, const std::string& fieldName) const;
    // May it write a field that owns objects (a class handle or a container), or reassign an object
    // parameter -- i.e. free an object that a field argument lives in?
    bool calleeMayMutateObjects(const std::string& bareName) const;
    // Does any function/method with this bare name have a read-only string parameter at `index`?
    // (for a call whose receiver class the emitter could not resolve)
    bool anyReadOnlyStringParamAt(const std::string& bareName, size_t index) const;

    // Is the function heap-pure: transitively, it frees or reallocates no container or object it does not
    // own locally, and replaces no object/container field or non-local variable. Writing a primitive field is
    // allowed. This is what makes a const& to a FIELD argument safe (see classifyHeapPureFunctions).
    bool isHeapPureFreeFunction(const std::string& fnName) const;

    // Same, for a class method.
    bool isHeapPureMethod(const std::string& className, const std::string& methodName) const;

    // Is a std::vector<primitive> parameter moved (not copied) at the one bare-name field assignment
    // (`data = param;`) that stores it, because that is provably its last use? See
    // classifyLastUseFieldAssignedParams for the deliberately narrow scope.
    bool isLastUseFieldAssignedParam(const std::string& fnName, const std::string& paramName) const;

    // Same classification for a class method's own parameter.
    bool isLastUseMethodFieldAssignedParam(const std::string& className, const std::string& methodName,
                                             const std::string& paramName) const;

private:
    // Finds value-type classes (see isValueTypeClass). Reads only the Program.
    void classifyValueTypeClasses(const Program& program);

    // First pass: collect all reference relationships
    void firstPassCollectReferences(const Program& program);
    void analyzeStatement(const Statement& stmt, const std::string& currentClass = "");
    void analyzeVariableDeclaration(const VariableDeclaration& decl, const std::string& currentClass);
    // Gives each object-typed parameter a minimal identity in variableReferences.
    void registerParameters(const std::vector<std::pair<std::string, std::string>>& params,
                            const std::string& scopeName);
    void analyzeAssignmentStatement(const AssignmentStatement& stmt, const std::string& currentClass);
    void analyzeClassDeclaration(const ClassDeclaration& cls);

    // Scans every push_back call site to decide each container variable's element pointer kind.
    // Self-contained, separate from the pointerDecisions machinery.
    void scanContainerElementKinds(const Program& program);
    void scanContainerElementKindsImpl(const Program& program);
    // scopeName is the scope of the statement's body (function name, "main" or "Class::method"); it is tried
    // before the global fallback when resolving an identifier's type.
    void scanContainerElementKindsInStatement(const Statement& stmt, const std::string& scopeName);
    // Every name declared in the body being scanned (locals of any type, loop variables, parameters). A name in this
    // set that has no entry in the body's own scope is a local of a type the symbol table does not record (string,
    // int, vector...), NOT the same-named class variable some other function happens to have.
    std::set<std::string> scanLocals_;

    // Object parameters and the object return of a virtual / override method are always SHARED: the
    // base declaration and every override must have the identical C++ signature, but each class's
    // pointer kinds are otherwise inferred from its own body, which cannot agree across a hierarchy.
    void forceVirtualSignaturesShared(const Program& program);
    // (scope, type, name, container) of every `container.push_back(identifier)` seen by the container scan
    std::vector<std::vector<std::string>> pushedArgs_;
    std::set<std::string> demandedFields_;     // "Class.name"
    std::set<std::string> demandedElements_;   // container field name
    std::set<std::string> demandedReturns_;    // "Class::method"
    // The declared type of every container field, by field name: tells the scan that a container holds
    // objects even in a file that never pushes into it.
    std::map<std::string, std::string> containerFieldTypes_;

    // Two handles to one object (`b = a;` then `a` used again) are both shared.
    void promoteAliasedLocals(const Program& program);
    void promoteAliasedLocalsInBody(const std::vector<StatementPtr>& body,
                                    const std::vector<std::pair<std::string, std::string>>& params,
                                    const std::string& scope);
    // A parameter stored into an already-shared field must itself be shared. Runs after the second pass.
    void promoteParamsStoredInSharedFields(const Program& program);
    void promoteParamsInBody(const std::vector<StatementPtr>& body, const std::string& className,
                             const std::vector<std::pair<std::string, std::string>>& params,
                             const std::string& scope);

    // A call argument bound to a parameter promoted to SHARED becomes SHARED too. Its statement/expression
    // walk lives in the .cpp as local lambdas.
    void promoteSharedCallArguments(const Program& program);

    // Infers which methods return an alias (shared_ptr) rather than transfer ownership: every reachable
    // `return` hands back `this`, an object field of the class, or a call to another alias-returning method.
    // A small fixed point per class. Writes methodReturnKinds_, SHARED hints for the fields returned, and
    // classesNeedingSharedFromThis_. Runs before secondPassDeterminePointerTypes.
    void inferAliasReturningMethods(const Program& program);

    // Promotes a method's return to SHARED if it returns a bare local whose decided kind is SHARED.
    // Needs every local's final decision, so it runs after promoteSharedCallArguments.
    void promoteReturnsOfSharedLocals(const Program& program);
    void promoteReturnsOfSharedLocalsOnce(const Program& program, bool& changed);
    bool localComesFromSharedCall(const Expression* init, const std::string& ownClass, const std::string& scope) const;

    // Free-function counterpart of inferAliasReturningMethods; runs after promoteReturnsOfSharedLocals.
    void inferAliasReturningFreeFunctions(const Program& program);

    // Classifies object parameters as read-only or not (see isReadOnlyFreeFunctionParam), as a fixed point
    // over the raw Program. Runs first, before any pointer kind exists. promoteSharedCallArguments later
    // forces both a read-only parameter and its call argument to SHARED.
    void classifyReadOnlyFreeFunctionParams(const Program& program);
    // Does `expr` use `paramName` in a way that consumes or stores it? okCallees holds the
    // "fnName::argIndex" targets known read-only so far in this round. currentClass is the enclosing class
    // ("" for a free function): a bare implicit-this call needs it to build the right okCallees key.
    // Reading only `paramName.field` never disqualifies.
    bool paramUseDisqualifiesReadOnly(const Expression* expr, const std::string& paramName,
                                       const std::string& currentClass,
                                       const std::set<std::string>& okCallees) const;

    // Container counterpart of classifyReadOnlyFreeFunctionParams, a separate fixed point because a
    // container's mutating method calls must disqualify.
    void classifyReadOnlyContainerParams(const Program& program);
    void classifyReadOnlyStringParams(const Program& program);
    // Does `expr` use the container parameter in a disqualifying way: assignment target, subscript write,
    // mutating method call, begin()/end(), copied into a local/field/return, closure capture, or passed bare
    // to a callee parameter that is not itself read-only? A subscript read, size() and a range-for over it
    // never disqualify (see bodyContainerHasGenuineRead).
    bool paramUseDisqualifiesReadOnlyContainer(const Expression* expr, const std::string& paramName,
                                                 const std::string& currentClass,
                                                 const std::set<std::string>& okContainerCallees) const;

    // Classifies heap-pure functions (see isHeapPureFreeFunction). A fixed point that starts from "pure" and
    // removes a function on evidence of impurity or an impure callee.
    void classifyHeapPureFunctions(const Program& program);
    // Does `body` contain an impurity: a container-mutating call on anything but a body-local container, an
    // assignment replacing an object/container field or non-local variable, or a call to a function not in
    // `pureSoFar` ("fn" or "Class::method")? localNames are the names the body declares (mutating them is
    // fine). primitiveFieldNames are the class's primitive fields: a bare write to one is fine, since a
    // primitive has no provenance.
    bool bodyHasImpurity(const std::vector<StatementPtr>& body, const std::string& currentClass,
                          std::set<std::string> localNames, const std::set<std::string>& pureSoFar,
                          const std::set<std::string>& primitiveFieldNames) const;

    // Classifies std::vector<primitive> parameters that are moved at their one field assignment (see
    // isLastUseFieldAssignedParam). A per-function scan, not a fixed point.
    void classifyLastUseFieldAssignedParams(const Program& program);
    // True only when `body` has exactly one bare-name field assignment `field = paramName;`, no statement
    // after it mentions paramName, and every statement is a declaration, assignment, expression, return or
    // throw. An if/while/for/try or closure anywhere in the body gives up.
    bool bodyIsLastUseFieldAssignment(const std::vector<StatementPtr>& body,
                                        const std::string& paramName) const;
    // Does the subtree mention `paramName`, or contain any closure (treated as a use)? Does not
    // distinguish reads from writes.
    bool exprReferencesName(const Expression* expr, const std::string& name) const;

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

    // container variable name -> true if some push_back pushed an already-shared source (the whole
    // container then holds shared_ptr). Absent or false means unique_ptr.
    std::map<std::string, bool> containerElementShared_;

    // className -> (object-typed fieldName -> its class), flattened across the inheritance chain.
    // analyzeClassDeclaration re-registers a base's fields under each derived class's scope.
    std::map<std::string, std::map<std::string, std::string>> classMembersByClass_;

    // "Class.method" -> inferred return pointer kind. Absent means UNIQUE (not SHARED, unlike the
    // field/local fallback).
    std::map<std::string, PointerType> methodReturnKinds_;

    // Classes that need enable_shared_from_this as a generated base.
    std::set<std::string> classesNeedingSharedFromThis_;

    // Value-type classes (see isValueTypeClass).
    std::set<std::string> valueTypeClasses_;

    // "fnName::paramName" -> read-only. Absent means not read-only (the parameter is moved in by value).
    std::set<std::string> readOnlyFreeFunctionParams_;
    // "className::methodName::paramName" -> read-only.
    std::set<std::string> readOnlyMethodParams_;

    // Same key shapes, for container parameters.
    std::set<std::string> readOnlyContainerParams_;
    std::set<std::string> readOnlyMethodContainerParams_;

    // "fnName" / "className::methodName" -> heap-pure.
    std::set<std::string> heapPureFreeFunctions_;
    std::set<std::string> readOnlyStringParams_;  // "fn::param" / "Class::method::param"
    std::map<std::string, std::set<std::string>> stringMutatedFieldsByFunction_;
    std::set<std::string> objectFieldNames_;
    std::set<std::string> readOnlyStringPositions_;  // "bare#index"
    std::set<std::string> heapPureMethods_;

    // Same key shapes, for the last-use field assignment classification.
    std::set<std::string> lastUseFieldAssignedParams_;
    std::set<std::string> lastUseMethodFieldAssignedParams_;
};

} // namespace cling
