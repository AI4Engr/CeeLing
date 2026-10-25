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
    // Copied out of / into a field (`Item c = p.child;`, `c = p.child;`,
    // `p.child = q.next;`): field and the other side are two live owners of one
    // object, so both must be shared -- unique_ptr cannot be copied.
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

class SmartPointerAnalyzer {
public:
    // Two-pass analysis
    void analyzeProgram(const Program& program, const std::string& source = "");
    PointerType getPointerType(const std::string& className, const std::string& variableName) const;
    // Scope-aware variant of getPointerType: the decision recorded for exactly this
    // (enclosing function/method, class, name) triple when there is one, else the merged
    // getPointerType answer. getPointerType merges EVERY same-named variable of the same class
    // across the whole program and answers SHARED when they disagree, so a function whose local
    // `item` is unique got a shared declaration just because another function's `item` is shared
    // -- while its return type, decided per function, stayed unique. The scope string is the one
    // analyzeStatement uses: a free function's name, "main", or "Class::method".
    PointerType getPointerTypeInScope(const std::string& scopeName, const std::string& className,
                                      const std::string& variableName) const;
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

    // Is `fieldName`, declared directly in class `enclosingClass`, lowered to a std::weak_ptr?
    // Exact (enclosing class, field) lookup -- unlike getPointerType, which matches on the
    // field's TYPE and silently falls back to SHARED when nothing matches. A weak_ptr field
    // cannot be read directly in C++ (no operator->, operator!, operator==, operator bool),
    // so every read of one has to go through lock(); AstEmitter asks this to know when.
    bool isWeakField(const std::string& enclosingClass, const std::string& fieldName) const;

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

    // Is free function `fnName`'s parameter `paramName` a READ-ONLY CONTAINER
    // (`std::vector<primitive>`)? A separate, stricter classification from
    // isReadOnlyFreeFunctionParam above (object-typed params): a container
    // parameter is read-only only if every use is a subscript READ, `.size()`,
    // the container of a range-for, or passed on bare to another already-
    // read-only container parameter (or an `ext_fn` `ref` parameter) -- NOT
    // "any non-disqualifying use", since a container has mutating METHOD
    // CALLS (`push_back`/`pop_back`/`resize`) that the object-typed rule's
    // receiver-of-a-method-call case would otherwise wave through as a mere
    // "read" (dispatch). See classifyReadOnlyContainerParams. Motivated by
    // zlib-cling's `decodeSymbol`, which copies four ~512-entry vectors on
    // every decoded symbol purely because CeeLing has no reference-parameter
    // syntax; measured 9.7x on zlib-cling's decompressor from passing those
    // four by `const&` alone.
    bool isReadOnlyContainerParam(const std::string& fnName, const std::string& paramName) const;

    // Same classification for a class method's own container parameter --
    // separate query for the same (class, method, param) triple-keying
    // reason isReadOnlyMethodParam is separate from isReadOnlyFreeFunctionParam.
    bool isReadOnlyMethodContainerParam(const std::string& className, const std::string& methodName,
                                          const std::string& paramName) const;

    // Is EVERY body statement of free function `fnName` (transitively,
    // through every callee it invokes) free of container reallocation/
    // destruction and of replacing an object/container field or variable
    // that is not its own local? This is what makes `const&` binding safe
    // for a NON-local argument (a field, or a chained field access): if the
    // callee is heap-pure, nothing it does can invalidate the reference the
    // caller's own field/vector is bound through. Writing a PRIMITIVE field
    // is fine (it cannot change or destroy a vector) -- only object/container
    // field/variable assignment and container-mutating method calls
    // disqualify. See classifyHeapPureFunctions.
    // A `string` parameter the body never mutates (see classifyReadOnlyStringParams): lowered to
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
    // Does ANY function/method with this bare name have a read-only string parameter at `index`?
    // (for a call whose receiver class the emitter could not resolve)
    bool anyReadOnlyStringParamAt(const std::string& bareName, size_t index) const;
    bool isHeapPureFreeFunction(const std::string& fnName) const;

    // Same, for a class method.
    bool isHeapPureMethod(const std::string& className, const std::string& methodName) const;

    // Is free function `fnName`'s `std::vector<primitive>` parameter
    // `paramName` safely MOVED (not copied) at the one bare-name field
    // assignment (`data = paramName;`) that stores it, because that
    // assignment is PROVABLY paramName's last use in the body? This is a
    // narrower, orthogonal question from isReadOnlyContainerParam above: a
    // read-only container is never moved anywhere (bound by `const&`
    // instead); this classification is for the opposite shape -- a
    // parameter whose ENTIRE body usage is "hand it to one field, and
    // never touch it again" (zlib-cling's `pushAll(std::vector<byte>
    // bytes) { input = bytes; }` is the motivating case: DanglingChecker's
    // own bare-field-store handling treats `data = bytes;` as a link/share
    // that leaves `bytes` live afterward -- correct in general (a linked-
    // list node is routinely used again right after being attached to a
    // field), but needlessly conservative for the common case where the
    // parameter genuinely never IS used again). See
    // classifyLastUseFieldAssignedParams for the scan and its deliberately
    // narrow scope (straight-line bodies only -- any if/while/for/try/
    // closure anywhere in the body means "give up", not "analyze the
    // branches").
    bool isLastUseFieldAssignedParam(const std::string& fnName, const std::string& paramName) const;

    // Same classification for a class method's own parameter.
    bool isLastUseMethodFieldAssignedParam(const std::string& className, const std::string& methodName,
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
    void scanContainerElementKindsImpl(const Program& program);
    // `scopeName` is the precise scope (free function name, "main", or
    // "Class::method") this statement's own body belongs to -- tried before
    // symbolTypesByScope's kGlobalScope fallback when resolving a bare
    // identifier's type, to avoid two unrelated functions' same-named
    // locals colliding in that fallback (see scanContainerElementKinds's
    // callers, which now pass this precisely).
    void scanContainerElementKindsInStatement(const Statement& stmt, const std::string& scopeName);
    // Every name declared in the body being scanned (locals of any type, loop variables, parameters). A name in this
    // set that has no entry in the body's own scope is a local of a type the symbol table does not record (string,
    // int, vector...), NOT the same-named class variable some other function happens to have.
    std::set<std::string> scanLocals_;

    // A parameter that a method/constructor body stores into an already-SHARED
    // field must itself be SHARED (a `shared_ptr` field can't hold a moved-in
    // `unique_ptr` and leave the source usable -- see dangling_checker.cpp's
    // AssignmentStatement field-store handling, which this mirrors). Self-
    // contained third pass, runs once after secondPassDeterminePointerTypes (needs
    // fields' own already-decided pointer kind), single pass (no fixed-point
    // iteration -- see .cpp for the documented limitation this leaves).
    // Two handles to one object (`b = a;` then `a` used again) are both shared. See the call site.
    // Object parameters and the object return of a virtual / override method are always SHARED: the
    // base declaration and every override must have the identical C++ signature, but each class's
    // pointer kinds are otherwise inferred from its own body (an unused parameter is unique, a read
    // one is shared), which cannot agree across a hierarchy.
    void forceVirtualSignaturesShared(const Program& program);
    // (scope, type, name, container) of every `container.push_back(identifier)` seen by the container scan
    std::vector<std::vector<std::string>> pushedArgs_;
    void promoteAliasedLocals(const Program& program);
    void promoteAliasedLocalsInBody(const std::vector<StatementPtr>& body,
                                    const std::vector<std::pair<std::string, std::string>>& params,
                                    const std::string& scope);
    void promoteParamsStoredInSharedFields(const Program& program);
    void promoteParamsInBody(const std::vector<StatementPtr>& body, const std::string& className,
                             const std::vector<std::pair<std::string, std::string>>& params,
                             const std::string& scope);

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

    // Container counterpart of classifyReadOnlyFreeFunctionParams -- a
    // SEPARATE fixed point (not folded into the one above) because the
    // per-use rule genuinely differs: a container's mutating method calls
    // (push_back/pop_back/resize) must disqualify, where the object-typed
    // rule's "receiver of a method call is a mere read" case would wrongly
    // let them through. Runs independently; does not depend on or feed
    // classifyReadOnlyFreeFunctionParams's own sets.
    void classifyReadOnlyContainerParams(const Program& program);
    void classifyReadOnlyStringParams(const Program& program);
    // Does expression `expr` use the bare identifier `paramName` (a
    // container-parameter candidate) in a way that disqualifies read-only
    // status? Disqualifying: assignment target, a subscript WRITE
    // (`paramName[i] = x`), a mutating method call receiver (push_back/
    // pop_back/resize), begin()/end(), being copied into a local/field/
    // return, closure capture, or passed bare to a callee parameter that is
    // not itself already read-only (container or ext_fn ref). A subscript
    // READ, `.size()`, and the container of a range-for are the genuine-read
    // evidence (see bodyContainerHasGenuineRead) and never disqualify.
    bool paramUseDisqualifiesReadOnlyContainer(const Expression* expr, const std::string& paramName,
                                                 const std::string& currentClass,
                                                 const std::set<std::string>& okContainerCallees) const;

    // Classifies which free functions/methods are HEAP-PURE (see
    // isHeapPureFreeFunction). A separate fixed point over the same
    // functions/methods: heap-purity of a function depends on the
    // heap-purity of everything it calls (a pure-looking function that
    // calls an impure one is itself impure), so this iterates to a fixed
    // point the same way the read-only classifications do, just with the
    // opposite monotonic direction (starts "assume pure", removes on
    // evidence of impurity or an impure callee, until nothing changes).
    void classifyHeapPureFunctions(const Program& program);
    // Does statement list `body` (within function `fnName`/method
    // `currentClass::methodName`) contain an impurity: a container-mutating
    // method call on anything but body-local container, an assignment that
    // replaces an object/container field or non-local variable, or a call to
    // a function/method not in `pureSoFar` (fixed-point set, "fnName" for
    // free functions / "Class::method" for methods)? `localNames` is the set
    // of names this body itself declared (VariableDeclaration) -- mutating
    // THOSE is fine, they cannot alias caller state. `primitiveFieldNames` is
    // the enclosing class's own primitive-typed field names (empty for a
    // free function) -- a BARE reassignment of one of these (CeeLing field
    // writes are bare names, e.g. `bitBuffer = bitBuffer >> n;`, not
    // `this->bitBuffer = ...`) is fine: a primitive has no provenance, so
    // writing one cannot change or destroy a container/object the caller can
    // see (confirmed necessary via a real gap: zlib-cling's dropBits/
    // needBits/bits, which only ever touch primitive bitBuffer/bitsUsed
    // fields this way, were wrongly classified impure without this,
    // defeating decodeSymbol's own call-site reference passing entirely).
    bool bodyHasImpurity(const std::vector<StatementPtr>& body, const std::string& currentClass,
                          std::set<std::string> localNames, const std::set<std::string>& pureSoFar,
                          const std::set<std::string>& primitiveFieldNames) const;

    // Classifies which std::vector<primitive> parameters are safely MOVED at
    // their one bare-name field-assignment statement (see
    // isLastUseFieldAssignedParam's own doc comment for the motivating case
    // and how this differs from the read-only classification). Not a fixed
    // point -- purely a per-function/method scan, independent of every other
    // function's own classification, since "is this the last use in MY body"
    // never depends on what any callee does.
    void classifyLastUseFieldAssignedParams(const Program& program);
    // Scans `body` (straight-line only -- see below) for a single bare-name
    // field-assignment statement (`fieldName = paramName;`, no `this->`/`.`
    // on the LHS, matching CeeLing's field-write syntax) whose RHS is
    // exactly the bare identifier `paramName`, and confirms no statement
    // AFTER that one references `paramName` again. Returns true only if:
    // exactly one such assignment exists (zero or more-than-one both give up
    // -- more than one field assignment from the same parameter is not
    // "one last use", it's an ambiguous shape this v1 does not attempt to
    // reason about), and every statement in `body` is one of
    // VariableDeclaration/AssignmentStatement/ExpressionStatement/
    // ReturnStatement/ThrowStatement -- an IfStatement/WhileStatement/
    // ForStatement/TryStatement ANYWHERE in the body (not just after the
    // candidate assignment) means "give up", not "analyze the branches":
    // this is deliberately far more conservative than
    // paramUseDisqualifiesReadOnlyContainer's per-use walk, trading recall
    // for a much smaller, easier-to-audit implementation, since the
    // motivating shape (zlib-cling's pushAll) is a single-statement body
    // and there is no evidence yet that a more general analysis is worth
    // building. A ClosureExpression anywhere (captures `paramName` by name,
    // not by reference, but still counts as "used") also gives up, checked
    // the same way.
    bool bodyIsLastUseFieldAssignment(const std::vector<StatementPtr>& body,
                                        const std::string& paramName) const;
    // Does the (expression) subtree contain a bare reference to `paramName`,
    // OR a ClosureExpression that captures `paramName` by name, OR a nested
    // ClosureExpression at all (conservatively treated as a use, regardless
    // of its own capture list, matching bodyIsLastUseFieldAssignment's
    // "closure anywhere gives up" rule)? Pure containment check -- unlike
    // paramUseDisqualifiesReadOnlyContainer, this never distinguishes a
    // "read" from a "write": ANY occurrence after the candidate assignment
    // disqualifies the move, full stop.
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

    // Same key shapes as above, for the container classification (see
    // isReadOnlyContainerParam/isReadOnlyMethodContainerParam).
    std::set<std::string> readOnlyContainerParams_;
    std::set<std::string> readOnlyMethodContainerParams_;

    // "fnName" / "className::methodName" -> heap-pure (see
    // isHeapPureFreeFunction/isHeapPureMethod).
    std::set<std::string> heapPureFreeFunctions_;
    std::set<std::string> readOnlyStringParams_;  // "fn::param" / "Class::method::param"
    std::map<std::string, std::set<std::string>> stringMutatedFieldsByFunction_;
    std::set<std::string> objectFieldNames_;
    std::set<std::string> readOnlyStringPositions_;  // "bare#index"
    std::set<std::string> heapPureMethods_;

    // Same key shapes as readOnlyContainerParams_/readOnlyMethodContainerParams_
    // above, for the last-use-field-assignment classification (see
    // isLastUseFieldAssignedParam/isLastUseMethodFieldAssignedParam and
    // classifyLastUseFieldAssignedParams).
    std::set<std::string> lastUseFieldAssignedParams_;
    std::set<std::string> lastUseMethodFieldAssignedParams_;
};

} // namespace cling
