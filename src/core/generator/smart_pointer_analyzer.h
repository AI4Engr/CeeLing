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
    // Scoped variant of getPointerTypeForVariable: only matches pointerDecisions
    // entries recorded under `scopeName` (a function/method name, matching
    // buildKey's own containingScope argument), never a same-named local in an
    // unrelated function. getPointerTypeForVariable's plain name-only suffix
    // match is a REAL, pre-existing cross-function name collision (confirmed:
    // two unrelated free functions each with their own local named `copy`
    // wrongly classified each other's return type) -- this scoped variant
    // exists specifically for promoteReturnsOfSharedLocals's two loops, which
    // always know which single function/method body they're asking about and
    // have no reason to risk a same-named local anywhere else in the program.
    PointerType getPointerTypeForVariableInScope(const std::string& scopeName,
                                                  const std::string& variableName) const;
    std::string buildAnalysisReport() const;
    const std::vector<AnalysisDiagnostic>& getDiagnostics() const;
    bool classRequiresSharedRoot(const std::string& className) const;

    // Phase 2.6 container support: a vector's element pointer kind must be uniform
    // (std::vector<unique_ptr<T>> and std::vector<shared_ptr<T>> are different C++
    // types), decided once across every push_back call site targeting that
    // container variable -- deliberately separate from the class-field/local
    // pointerDecisions machinery above (self-contained, so it can't perturb those
    // existing heuristics; see the .cpp for the exact rule). WEAK is never an
    // element kind (back-reference detection is field-name-based and doesn't apply
    // to container elements in this slice).
    // Is `containerVarName`'s element kind SHARED (true) or UNIQUE (false, the
    // default when no push_back call forces sharing)?
    bool containerElementIsShared(const std::string& containerVarName) const;

    // A method's RETURN pointer kind, INFERRED (never annotated -- see
    // inferAliasReturningMethods for the exact rule) from its body's own
    // shape: a method whose every `return` hands back `this` or an object-
    // typed field (never `new T(...)`) is classified ALIAS_RETURNING and gets
    // SHARED here. Deliberately defaults to UNIQUE (not SHARED, unlike
    // getPointerType's field/local fallback), so an ordinary object-returning
    // method keeps today's unique_ptr/move-return behavior completely
    // unchanged unless its body actually matches the alias-returning shape.
    PointerType getMethodReturnPointerType(const std::string& className,
                                           const std::string& methodName) const;

    // Does `className` need `std::enable_shared_from_this<className>` as a
    // compiler-generated base? True iff some ALIAS_RETURNING method's body
    // contains a bare `return this;` -- auto-derived from the same inference
    // pass, no user-facing annotation exists for this at all.
    bool classNeedsSharedFromThis(const std::string& className) const;

    // Is `className` a VALUE TYPE -- never wrapped in unique_ptr/shared_ptr for
    // its own variable declarations, class fields, or method return values,
    // unlike every other user-defined class? Purely structural, no annotation
    // (see classifyValueTypeClasses): true iff the class defines all three of
    // operator*/operator++/operator!= (the C++ range-for iterator protocol).
    // This is a SEPARATE, independent classification from PointerType --
    // AstEmitter checks this FIRST and, if true, skips calling
    // getPointerType/getMethodReturnPointerType entirely for that class-typed
    // thing (a value-type class's own FIELDS of some OTHER class type are
    // unaffected and still go through the ordinary pointer-kind decision).
    bool isValueTypeClass(const std::string& className) const;

    // Is free function `fnName`'s parameter `paramName` READ-ONLY -- never
    // stored into a field/return/another owning sink anywhere in the body,
    // only ever read via `paramName.field` (including passing such a field
    // access on to another already-classified read-only parameter)? See
    // classifyReadOnlyFreeFunctionParams for the exact structural rule.
    // Motivated by yaml-cpp API-compatibility: a yaml-cpp function taking
    // `const Node&` (e.g. `Clone`) must not consume its caller's argument,
    // even though CeeLing has no reference-parameter syntax and an object
    // parameter defaults to move-by-value.
    bool isReadOnlyFreeFunctionParam(const std::string& fnName, const std::string& paramName) const;

    // Same classification, for a CLASS METHOD's own parameter (e.g. Parser::
    // parseValueInto's `target`, which is only ever mutated THROUGH -- never
    // stored/returned/moved-into-another-owner -- so the caller's own local
    // must survive the call). Kept as a separate query (not folded into
    // isReadOnlyFreeFunctionParam) since methods key by (class, method, param)
    // triples, not just (fnName, paramName) pairs -- two different classes'
    // same-named methods must not collide. See classifyReadOnlyFreeFunctionParams,
    // which now classifies both free functions and methods in one fixed point.
    bool isReadOnlyMethodParam(const std::string& className, const std::string& methodName,
                                const std::string& paramName) const;

private:
    // Classifies value-type classes (see isValueTypeClass) by scanning every
    // ClassDeclaration's members for operator*/operator++/operator!= method
    // definitions. Fully independent of the rest of the pipeline -- reads
    // only the raw Program, writes only valueTypeClasses_ -- so it can run
    // first, before firstPassCollectReferences, with no ordering dependency.
    void classifyValueTypeClasses(const Program& program);

    // First pass: collect all reference relationships
    void firstPassCollectReferences(const Program& program);
    void analyzeStatement(const Statement& stmt, const std::string& currentClass = "");
    void analyzeVariableDeclaration(const VariableDeclaration& decl, const std::string& currentClass);
    // Give each object-typed parameter a minimal identity in variableReferences (see
    // .cpp for why this stays deliberately minimal -- no assignment/back-reference
    // heuristics wired in).
    void registerParameters(const std::vector<std::pair<std::string, std::string>>& params,
                            const std::string& scopeName);
    void analyzeAssignmentStatement(const AssignmentStatement& stmt, const std::string& currentClass);
    void analyzeClassDeclaration(const ClassDeclaration& cls);

    // Phase 2.6: scan every push_back call site across the whole program (mirrors
    // the top-level firstPassCollectReferences walk, but self-contained -- own
    // data, own recursive statement walk) to decide each container variable's
    // element pointer kind. Runs once per analyzeProgram call.
    void scanContainerElementKinds(const Program& program);
    void scanContainerElementKindsInStatement(const Statement& stmt);

    // A parameter that a method/constructor body stores into an already-SHARED
    // field must itself be SHARED (a `shared_ptr` field can't hold a moved-in
    // `unique_ptr` and leave the source usable -- see dangling_checker.cpp's
    // AssignmentStatement field-store handling, which this mirrors). Self-
    // contained third pass, runs once after secondPassDeterminePointerTypes (needs
    // fields' own already-decided pointer kind), single pass (no fixed-point
    // iteration -- see .cpp for the documented limitation this leaves).
    void promoteParamsStoredInSharedFields(const Program& program);
    void promoteParamsInBody(const std::vector<StatementPtr>& body, const std::string& className,
                             const std::vector<std::pair<std::string, std::string>>& params);

    // A call-site argument bound to a parameter that sub-pass 1 promoted to SHARED
    // must itself become SHARED (otherwise the caller's local is still emitted as
    // unique_ptr and the promotion is invisible at the call site). Runs after
    // promoteParamsStoredInSharedFields. Its own recursive statement/expression
    // walk lives entirely inside the .cpp (as local lambdas, not member functions)
    // since analyzeStatement doesn't visit call expressions, control-flow bodies,
    // or constructor-call (NewExpression) arguments at all, and nothing else in
    // this class needs that walk.
    void promoteSharedCallArguments(const Program& program);

    // INFERS which methods return an ALIAS (shared_ptr) rather than transfer
    // ownership (unique_ptr, the default), purely from each method body's own
    // shape -- no annotation. A method is classified ALIAS_RETURNING iff every
    // reachable `return` in it hands back `this`, an object-typed field of the
    // same class, or a call to ANOTHER already-classified alias-returning
    // method (the recursive-walker shape, e.g. yaml-cpp-style
    // `return child.findOrAppendSibling(key);`). Runs as a genuine small
    // fixed-point over each class's own methods (a "leaf" method like
    // `siblingAt` must be classified before a caller like `operator[]` that
    // depends on it), self-contained (own data, own recursive statement walk,
    // mirrors scanContainerElementKinds's shape) -- see the .cpp for the exact
    // per-round algorithm. Writes classification into methodReturnKinds_, and
    // a field found under case (b) into explicitHints (SHARED) so it flows
    // through the exact same lookup path every other field decision already
    // uses. Also auto-derives classesNeedingSharedFromThis_ for any
    // alias-returning method whose body contains a bare `return this;`. Runs
    // BEFORE secondPassDeterminePointerTypes (so its explicitHints writes are
    // picked up by that pass's applyExplicitHints call).
    void inferAliasReturningMethods(const Program& program);

    // A FOURTH pass, running after promoteSharedCallArguments (needs every
    // local's FINAL pointer-kind decision): promotes a method's return
    // classification to SHARED if it returns a bare local variable whose own
    // decided kind is SHARED, even though inferAliasReturningMethods's
    // structural rule (this/field/alias-call only) never covers plain locals.
    // See the .cpp for the concrete yaml-cling Parser shape this closes.
    void promoteReturnsOfSharedLocals(const Program& program);

    // Free-function counterpart of inferAliasReturningMethods's case (c) --
    // MUST run AFTER promoteReturnsOfSharedLocals (needs a called class
    // method's FINAL classification, which that pass may still be setting).
    // See the .cpp for the exact yaml-cling Load() shape this closes.
    void inferAliasReturningFreeFunctions(const Program& program);

    // Classifies each free function's object-typed parameters as read-only or
    // not (see isReadOnlyFreeFunctionParam). Purely structural over the raw
    // Program (mirrors classifyValueTypeClasses's independence) -- runs FIRST,
    // alongside it, before any pointer-kind decision exists yet, since this
    // classification is about how a PARAMETER'S OWN BODY USAGE shapes it, not
    // about any already-decided pointer kind. promoteSharedCallArguments
    // consults this (much later) to force both a read-only parameter AND its
    // call-site argument to SHARED -- see the .cpp for why both sides need it.
    void classifyReadOnlyFreeFunctionParams(const Program& program);
    // Does expression `expr`, within a function body where `paramName` names a
    // read-only-candidate parameter, use `paramName` in a way that would
    // require it to be consumed/stored (fails read-only), given `okCallees` --
    // the set of "fnName::argIndex" call targets already known read-only this
    // round (a fixed point, mirroring inferAliasReturningMethods's algorithm,
    // since a parameter passed onward to another read-only parameter is itself
    // still read-only, but that target might not be classified yet on an
    // earlier round)? Returns true the first time a disqualifying use is
    // found; a call site that reads only `paramName.field` (not bare
    // `paramName`) never disqualifies, regardless of the callee.
    // `currentClass` is the enclosing class of the body being scanned (empty
    // for a free function) -- needed so a bare implicit-this call inside a
    // METHOD (`parseBlockMapInto(target, ...)`, indistinguishable at the AST
    // level from a free-function call: call->callee is null either way) can
    // build the right okCallees key ("Class::method::argIndex", not just
    // "method::argIndex", which would collide across classes and never match
    // a free function's own differently-shaped key).
    bool paramUseDisqualifiesReadOnly(const Expression* expr, const std::string& paramName,
                                       const std::string& currentClass,
                                       const std::set<std::string>& okCallees) const;

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

    // container variable name -> true if any push_back call site pushed an
    // already-SHARED source (forcing the whole container's element kind to
    // shared_ptr). Absent/false = unique_ptr (the default).
    std::map<std::string, bool> containerElementShared_;

    // className -> (object-typed fieldName -> its class), FLATTENED across the
    // inheritance chain. Used by analyzeClassDeclaration to re-register a base's
    // fields under each derived class's own scope (see the comment there for why
    // this analyzer needs per-scope re-registration rather than a shared flat map).
    std::map<std::string, std::map<std::string, std::string>> classMembersByClass_;

    // "Class.method" -> the method's INFERRED return pointer kind (see
    // inferAliasReturningMethods). Absent means "not alias-returning" --
    // getMethodReturnPointerType then defaults to UNIQUE, NOT SHARED, unlike
    // the field/local pointerDecisions fallback.
    std::map<std::string, PointerType> methodReturnKinds_;

    // Classes needing `std::enable_shared_from_this<T>` as a compiler-generated
    // base, auto-derived (never user-annotated) from an alias-returning
    // method's body containing `return this;`.
    std::set<std::string> classesNeedingSharedFromThis_;

    // Classes classified as value types (see isValueTypeClass/
    // classifyValueTypeClasses).
    std::set<std::string> valueTypeClasses_;

    // "fnName::paramName" -> read-only (see isReadOnlyFreeFunctionParam).
    // Absence means "not read-only" (the safe default -- an ordinary,
    // unclassified parameter keeps today's move-by-value behavior unchanged).
    std::set<std::string> readOnlyFreeFunctionParams_;
    // "className::methodName::paramName" -> read-only (see isReadOnlyMethodParam).
    std::set<std::string> readOnlyMethodParams_;
};

} // namespace cling
