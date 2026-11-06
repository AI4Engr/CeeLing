#pragma once

#include "../cst_to_ast/ast.h"
#include "smart_pointer_analyzer.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cling {

// A single dangling / use-after-move diagnostic, anchored to a .clg source line.
struct DanglingError {
    int line = 0;
    int column = 0;
    std::string message;
    std::string help;
    // The `semantic/<code>` category printed with the error; every error of this checker used to be
    // labelled use-after-move, which was wrong for the ones that are not.
    std::string code = "use-after-move";
};

// Minimal single-pass use-after-move / dangling checker (Phase 2 seed).
//
// Scope of THIS version (deliberately small — the first fixture to light up):
//   * "move" events: passing an object-typed local as a function-call argument, and
//     assigning one object-typed local from another (`Node b = a;` moves a).
//   * "use" check: any later use of a moved-from variable is an error.
// Auto-derived return-provenance summaries (the design notes component A) are also
// implemented: CeeLing bans `ref` function parameters, so every object-typed
// parameter is unconditionally moved into the callee -- there is no per-parameter
// borrow/move case to summarize. The only thing that varies per-function is whether
// the RETURN value aliases one of its own parameters (a passthrough, e.g.
// `T f(T a){ return a; }`) or is always a fresh/owned value. `deriveSummaries`
// computes this once per function/method before the main check; call sites that
// assign a call result consult it to register the right provenance instead of
// assuming "always fresh".
// Return-local-reference (#2) is also handled: CeeLing has no `ref` return type, so
// returning an observer name always hands back a reference into the returning
// function's own stack frame -- rejected by checkReturnedReference regardless of
// whether the generated C++ would happen to also fail to compile.
// Not yet handled: library/std/Eigen summaries, field-escape (#3), reassignment
// revival across branches (branch-merge precision). Those arrive in later increments.
//
// Conservative by construction: it only flags a use that provably follows a move on
// the linear statement order within one function body. It never rewrites code and has
// no false-positive path for the constructs it does not model (they are simply
// ignored, not rejected — tightening comes later).
class DanglingChecker {
public:
    // Returns all detected errors (empty => program is accepted by this checker).
    // `analyzer` must already have run analyzeProgram over the same `program` --
    // needed for Phase 2.6 container support: whether a specific `push_back`
    // argument is shared or moved depends on the argument's own already-decided
    // pointer kind (unique vs shared), which only SmartPointerAnalyzer knows (see
    // applyCallArgMoves and Chapter 18's note on this cross-module dependency).
    std::vector<DanglingError> check(const Program& program, const SmartPointerAnalyzer& analyzer);

private:
    // Auto-derived per-function/method borrow contract (the design notes component A).
    // CeeLing bans `ref` parameters, so every object-typed parameter is always moved
    // by the callee -- the only thing that varies per-function is what the return
    // value aliases, if anything.
    struct FunctionSummary {
        // Index into the parameter list that the return value aliases (borrows
        // from), or -1 if the return value is always a fresh/owned object (or the
        // function is void/non-object-returning).
        int returnBorrowsParam = -1;
    };

    void collectClassNames(const Program& program);

    // One pass over the whole program computing a FunctionSummary for every free
    // function and method, before any body is checked -- so forward references
    // within the same file already have a summary available.
    void deriveSummaries(const Program& program);
    FunctionSummary deriveSummaryForBody(
        const std::vector<StatementPtr>& body,
        const std::vector<std::pair<std::string, std::string>>& params) const;

    // Qualified summary key: free functions by name, methods by "ClassName::name".
    static std::string summaryKey(const std::string& ownerClass, const std::string& name) {
        return ownerClass.empty() ? name : ownerClass + "::" + name;
    }

    // Analyze one function/method body: clears per-body state, registers parameters
    // (object-typed ones become live owners, same as a VariableDeclaration would),
    // then processes each statement in order. `ownerClass` is the enclosing class for
    // methods ("" for free functions/main), used to resolve method-call summaries.
    void checkBody(const std::vector<StatementPtr>& body,
                    const std::vector<std::pair<std::string, std::string>>& params,
                    const std::string& ownerClass, const std::string& scope);

    // Process one statement in the CURRENT scope (does not clear state). Nested blocks
    // (e.g. an `if` body) reuse the enclosing scope so outer observers stay visible;
    // owners declared inside the block die when the block ends.
    void processStatement(const Statement* stmt);

    // Owners declared at the end of a block die: mark their observers dangling and drop
    // them from the live set. `ownersBefore` is the object-local set captured on entry.
    void endBlockScope(const std::set<std::string>& ownersBefore, const std::set<std::string>* stringsBefore = nullptr);

    // `string_view` (a non-owning view of a string). A view is an observer of the string local or parameter it was made from, registered in
    // observerOf_ like any observer: reassigning the string, a mutating method on it (stringMethodInvalidatesReferences) or the end of its scope
    // kills the view, and any later use of the view is rejected. A view of a literal or of a view parameter has no owner here (static storage /
    // the caller's, alive for the whole call). Anything else a view could be made from (a call result, a field, a concatenation) is refused:
    // nothing could say what it refers to.
    std::set<std::string> stringOwners_;   // string locals and parameters currently in scope
    void bindStringView(const std::string& name, const Expression* init, const Statement* at);
    // A `span<T>` (library_summary.h) is the same kind of observer, of a vector local or parameter: the vector's reassignment, a method that
    // may reallocate it (vectorMethodInvalidatesReferences), its being moved away and the end of its scope kill the span. It can be made from
    // such a vector, from another span, or from first/last/subspan of a span; anything else (a call result, a field) is refused.
    void bindSpan(const std::string& name, const Expression* init, const Statement* at);

    // Snapshot of every piece of state a branch body can mutate that must NOT leak
    // into a sibling branch that didn't run (only one branch of an `if`/`else` ever
    // executes). `movedFrom_` was already snapshotted/restored per branch; the
    // observer-tracking maps were not, which let a rebind inside one branch (e.g.
    // `obs = b;`) pollute the OTHER branch's view of what `obs` observes -- the same
    // class of bug fixed for `movedFrom_` earlier, just not extended to these fields.
    struct BranchState {
        std::set<std::string> movedFrom;
        std::set<std::string> observerNames;
        std::map<std::string, std::string> observerOf;
        std::set<std::string> danglingObservers;
        std::set<std::string> aliasObservers;
    };
    BranchState snapshotBranchState() const;
    void restoreBranchState(const BranchState& state);
    // Conservative union-merge of two branch outcomes into the current state (used
    // after both an `if` and its `else` have been independently analyzed from the
    // same starting snapshot). A name/observer affected in EITHER branch keeps that
    // effect afterward (can't know which branch ran) -- same "ambiguous ->
    // conservative reject" rule already used for movedFrom_, extended to observers:
    // if the two branches disagree about what an observer observes (or one killed it
    // and the other didn't), the observer is dangling after the merge point.
    void mergeBranchStates(const BranchState& before, const BranchState& thenState,
                            const BranchState& elseState);

    // One entry per `while`/`for` currently being analyzed (innermost last). A `break` or
    // `continue` leaves the straight-line flow, so its effects (a move, an observer rebind, a
    // dying owner) must not reach the statements after it in the body -- the `if` merge treats
    // such a branch as exiting -- but they DO reach the code after the loop, which is where both
    // statements lead (`continue` via the loop condition failing). Each one records the state it
    // left behind here, and the loop merges every recorded state into its post-loop state.
    struct LoopContext {
        std::set<std::string> ownersBefore;     // object locals declared before the loop
        std::vector<BranchState> breakExits;    // state at each `break`: leaves the loop only
        std::vector<BranchState> continueExits; // state at each `continue`: next iteration or exit
    };
    std::vector<LoopContext> loopStack_;
    // Record the state at a break/continue (called with the loop's own scope still open, so owners
    // declared inside the loop are marked dead for their observers in the recorded copy only).
    void recordLoopExit(bool isContinue);
    // Analyse a `while` (condition != nullptr) or range-`for` (forStmt != nullptr) body. The body runs
    // more than once, so it is analysed again from the state the previous pass left at the loop head
    // (fall-through and `continue` flow back; `break` does not) until that state stops changing --
    // otherwise `use(a); sink(a);` inside a loop passes although the second iteration uses a moved
    // `a`. The state after the loop is the union of: no iteration ran, every pass's end, every break
    // and every continue.
    void processLoop(const Expression* condition, const ForStatement* forStmt,
                     const std::vector<StatementPtr>& body);

    // Record which locals are object-typed (only those participate in move tracking).
    // Returns true if `type` names a known class OR a recognized container type
    // (e.g. `std::vector<Node>` -- see library_summary.h). A container's own
    // lifetime matters exactly like a class object's (moving it moves the backing
    // store; an observer of a moved-from/killed container dangles), so it reuses
    // 100% of the existing move/observer machinery once recognized here.
    bool isObjectType(const std::string& type) const;
    // True if `name` is an object local whose pointer kind resolved to SHARED: assigning it onward
    // copies the handle (the emitter does exactly that), so it is not consumed.
    bool isSharedLocal(const std::string& name) const;

    // Is `expr` provably a `string`-value expression? A string literal always is;
    // a `string`-typed identifier is (via localType_); a `+` BinaryExpression of
    // two string-valued operands is (string concatenation has no ownership/
    // dangling implications -- it always produces a fresh value, same as any other
    // primitive arithmetic). Recursive so `a + b + c` (left-associative chains)
    // resolves correctly. Used by ThrowStatement's operand check -- see the design notes/
    // The internal notes on why `throw` is restricted to string payloads at all.
    bool isStringValuedExpr(const Expression* expr) const;

    // Walk an expression; for every use of a moved-from variable, emit an error.
    void checkExpressionUses(const Expression* expr);
    // Opaque external classes (`opaque X` in a fact pack): a call to a method with no fact kills every observer of its receiver.
    // Walks the whole expression, nested calls included, BEFORE any use in it is checked (C++ leaves the evaluation order of
    // operands unspecified, so a use and a kill in one expression are treated as kill first).
    void killObserversOfOpaqueCalls(const Expression* expr);
    // `Grid w = g.merge(h);` with `merge` unlisted on an opaque class: the result may borrow `h` as well as `g`, and an observer follows ONE
    // owner. Refused (the pack can say `fresh Grid::merge` or `borrow Grid::merge`) instead of guessing.
    void rejectOpaqueResultWithObjectArgs(const Expression* init, const Statement* at);
    // `Grid w = field.window(..)` / `Grid w = holder.field.window(..)`: the result borrows a FIELD (of an external class, borrowing by fact or by the
    // opaque default). A field has no owner this analysis follows, so the dependence would be silently dropped: refuse it instead.
    void rejectBorrowFromFieldReceiver(const Expression* init, const Statement* at);
    int exprDepth_ = 0;

    // If `expr` is a bare object-typed local, return its name; else empty.
    std::string asObjectLocalName(const Expression* expr) const;

    // If `expr` is a subscript into a container-typed local (`v[i]`), return the
    // CONTAINER's name (not a per-element identity -- an element has no name of its
    // own; invalidation is a whole-container reallocation event, so provenance is
    // tracked at the container level, matching threat #4's actual shape). Else "".
    // Used the same way asObjectLocalName is used for observer-binding: `ref Node n
    // = v[i];` should make `n` observe `v`, and `v.push_back(...)` (see
    // invalidateContainerObservers) should be able to find every such `n`.
    std::string asContainerSubscript(const Expression* expr) const;

    // If `expr` is a bare identifier or a subscript, return the name of the object-
    // typed local it resolves to for observer-binding purposes (asObjectLocalName
    // for a bare identifier, asContainerSubscript for `v[i]`). Shared choke point
    // for every "what does this ref/alias observe" call site.
    std::string resolveObservedOwner(const Expression* expr) const;

    // Binding a `ref` (declaration or rebind) is only sound when the checker can
    // track the observed object's owner: an object local, `v[i]` of a container
    // local, another observer (inherits its owner), or `nullptr`. Anything else --
    // a field path (`p.child`), a call result (`p.getChild()`, `make()`) -- is
    // lowered to a raw `.get()` pointer whose owner no death event will ever reach,
    // so it is rejected here. On success `owner` is the tracked owner ("" for
    // nullptr / an observer that has none yet) and `inheritsDangling` says the
    // source observer was already dangling.
    bool resolveRefBinding(const Expression* expr, std::string& owner,
                           bool& inheritsDangling) const;
    void reportUntrackableRefBinding(const std::string& name, const Expression* expr,
                                     const ASTNode* anchor);

    // Is `assign` a bare-identifier target of the form `n = value;` where `n`'s
    // declared type is a class that declares its own `operator=`, and `value`
    // isn't itself an object local of that same class? True means this is really
    // `n->operator=(value)`, a MUTATING call on the still-alive object, not
    // ownership reassignment -- the AssignmentStatement branch must skip the
    // owner-death/move logic for it.
    bool isOperatorAssignMutation(const AssignmentStatement& assign) const;

    // A known invalidating library call (only `push_back` this slice -- see
    // library_summary.h) kills every existing observer of its container receiver:
    // the backing store may have reallocated, so any `ref`/subscript-derived
    // reference into the old storage is now dangling. Structurally identical to
    // what endBlockScope already does for scope-exit and the AssignmentStatement
    // branch already does for reassignment -- just a third death-event source,
    // reusing the same danglingObservers_ set (already conservatively unioned
    // across branches by mergeBranchStates, so this needs no new BranchState field).
    void invalidateContainerObservers(const Expression* callExpr);
    // A span of a FIELD vector of the current class (`std::span<int> s = items;` in a method) observes the pseudo owner "@items". Nothing follows
    // a field across calls, so such a span dies at: an assignment to a field of that name, a vector mutator called on that field, and any call
    // that may reach the heap (a user function or method that is not heap-pure, or a `new`). Passing such a span to a call that may change the
    // field is refused.
    void killFieldObservers(const std::string& field);      // "" = every field observer
    bool hasFieldObservers() const;
    // 0: not user code (a library call), 1: every user function/method of that name is heap-pure, 2: some is not
    int calleeHeapEffect(const FunctionCall* call) const;

    // Resolve the CeeLing CLASS NAME of `expr`'s static type -- a bare identifier
    // (local/param/field, via localType_/classFields_) or a CHAINED call
    // (`a.b()`/`f()`, via functionReturnType_/classMethodReturnType_, recursing
    // into the call's own receiver). Mirrors
    // EscapeHatchChecker::resolveObjectType's identical widening and identical
    // safety argument: every CeeLing object is compiler-wrapped in a smart
    // pointer, so a method/function's return value is always a live, owning
    // COPY, never a raw alias into an already-destroyed temporary -- widening
    // return-type resolution to a call receiver cannot itself introduce a
    // dangling read. Returns "" for anything unresolved (`new`, literals, an
    // external/opaque type not in classFields_/classMethodParams_, etc.),
    // preserving the existing conservative fallback everywhere this is used.
    std::string resolveClassOfExpr(const Expression* expr) const;

    // If `expr` is a call to a known CeeLing function/method whose summary says the
    // return value aliases one of its parameters, return the NAME of the object-local
    // argument passed at that position (empty if not applicable: unknown callee,
    // summary says "fresh", or the aliased argument isn't a bare object local).
    std::string resolveCallReturnAlias(const Expression* expr) const;

    // If `expr` is `receiver.method(args)` where `receiver`'s declared type is a
    // known-external class (e.g. Eigen::MatrixXd) and `method` is an allow-listed
    // BORROWING method (isKnownBorrowingMethod -- `.block()`, `.row()`, ...),
    // return `receiver`'s bare identifier name (empty otherwise). Distinct from
    // resolveCallReturnAlias: that resolves borrow-by-PARAMETER for CeeLing's own
    // functions (populated via classMethodParams_/summaries_, which only exist for
    // a real parsed ClassDeclaration); a known-external class has no such
    // declaration for anything to populate those maps from, so this is a separate,
    // parallel path keyed directly off library_summary.h's allow-list. Also
    // semantically different: the receiver here is NOT moved/consumed by the call
    // (`a.block(...)` leaves `a` fully usable -- only a LATER reassignment/move of
    // `a` should kill the borrowed result), unlike an aliased call ARGUMENT, which
    // the existing alias-observer machinery assumes was already moved into the
    // callee by construction. Callers must NOT add the resulting observer to
    // aliasObservers_ for this reason -- see processStatement's use of this
    // function for how that distinction is preserved.
    std::string resolveBorrowingMethodReceiver(const Expression* expr) const;

    // If `expr` is a call, every object-typed-local argument is moved into the
    // callee (CeeLing bans `ref` parameters, so a call always consumes its object
    // arguments). No-op for non-call expressions.
    // Applies the moves and invalidations of EVERY call inside `expr`, in evaluation order (operands and arguments before the call that
    // takes them), however deep: `f(v) + g(v)`, `h(f(v))`, `std::cout << f(v)`. A call that moves an owner the same statement already moved
    // is a use of a moved-from value.
    void applyCallArgMoves(const Expression* expr);
    void applyOneCallMoves(const Expression* expr);      // the moves and invalidations of the single call `expr`

    // `cling::spawn(new __closure_N(captures...))`: decides which object captures may be MOVED into the
    // new thread and marks each one moved-from. Rejects the rest. See the rules in the .cpp.
    void checkSpawnCall(const FunctionCall* call);
    // The reason object `name` of class `type` may NOT be moved into a thread, or "" if it may.
    std::string spawnObjectProblem(const std::string& type, const std::string& name) const;

    // Return-local-reference check (#2): CeeLing has no `ref` return type, so a
    // function's return is always owning -- returning a `ref` observer hands back a
    // reference into THIS function's own stack frame, which is gone the instant the
    // function returns. Rejects `return b;` when `b` is an observer declared in the
    // current body (parameters can't be observers -- `ref` params are already banned
    // at parse time -- so any observer name here was necessarily declared locally).
    void checkReturnedReference(const Expression* expr);

    std::set<std::string> classNames_;
    // className -> set of that class's field names. Lets processStatement recognize
    // `link = other;` (bare identifier, implicit `this`) inside a method/constructor
    // body as a field-store (link/share semantics), not a local reassignment --
    // mirrors EscapeHatchChecker::classFields_, built once before any body is checked.
    std::map<std::string, std::set<std::string>> classFields_;
    // className -> field name -> declared type text (for resolving a receiver such as `hub.inbox` to `Channel<Req>`).
    std::map<std::string, std::map<std::string, std::string>> classFieldTypes_;
    // The declared type of a receiver expression: a local / parameter, a field of the current class, or `obj.field` of a known class; "" if unknown.
    std::string declaredTypeOf(const Expression* expr) const;
    // Qualified summary key (summaryKey()) -> derived summary.
    std::map<std::string, FunctionSummary> summaries_;
    // Free-function name -> declared parameter list (needed to resolve a call's
    // aliased-argument expression by index). Methods are looked up per-receiver-class
    // via classMethodParams_.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> functionParams_;
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, std::string>>>> classMethodParams_;
    // Free-function name / (className -> methodName) -> declared return type.
    // Lets resolveClassOfExpr (see below) resolve a CHAINED receiver
    // (`a.b().c()`: needs `b()`'s return class before `c()`'s own summary can be
    // looked up) the same way EscapeHatchChecker::classMethodReturnType_/
    // freeFunctionReturnType_ do -- kept as this checker's OWN copy rather than
    // shared with EscapeHatchChecker (which runs as a fully separate pass over
    // its own class/method tables) to avoid a cross-checker dependency.
    std::map<std::string, std::string> functionReturnType_;
    std::map<std::string, std::map<std::string, std::string>> classMethodReturnType_;

    // (className, methodName) -> index of the parameter that is a MUTABLE
    // OUT-PARAMETER, not moved into the callee -- populated for a method
    // whose PRIMARY generic uses its type parameter T as a parameter type
    // (`<T> bool decodeInto(T out)`, see ASTBuilder::buildMethodDeclaration's
    // T-in-param-position support and the design notes' convert<T> section). Every
    // concrete specialization of such a primary (`bool decodeInto<Point>(Point
    // out)`) shares the SAME out-parameter contract at the SAME index by
    // construction -- an explicit specialization's parameter list must match
    // the primary's shape, so there is exactly one out-parameter index per
    // (class, method name), independent of which concrete specialization a
    // given call resolves to. NOTE: this is unrelated to the design notes'
    // roadmap `borrow<T>` item (a future, optional, general user-facing
    // ownership annotation, not yet designed) -- this mechanism is a much
    // narrower, mechanical special case that only ever arises from this one
    // structural pattern (a generic's T used in parameter position), and is
    // never exposed as an annotation a user can apply elsewhere.
    std::map<std::string, size_t> outParamIndex_;
    static std::string outParamKey(const std::string& ownerClass, const std::string& methodName) {
        return ownerClass + "::" + methodName;
    }
    // ext_fn name -> indices of its `ref` parameters (see
    // ExternFunctionDeclaration::refParams in ast.h). applyCallArgMoves
    // consults this, exactly like outParamIndex_ above, to skip inserting a
    // `ref` argument into movedFrom_ -- the argument is borrowed for the
    // call's duration, not moved, so it stays a live, usable owner
    // afterward. ext_fn is always a free function (never a method), so this
    // is keyed by plain name only, unlike outParamIndex_'s (class, method)
    // key.
    std::map<std::string, std::set<size_t>> externRefParams_;

    std::string currentClass_;  // enclosing class of the body being checked ("" = free fn)
    std::string currentScope_;  // analyzer scope string of the body being checked (see getPointerTypeInScope)
    std::set<std::string> objectLocals_;   // object-typed owners live in current body
    std::map<std::string, std::string> localType_;  // object-local/param name -> class name
    std::set<std::string> movedFrom_;      // moved-from locals in the current body
    std::set<std::string> observerNames_;  // all names declared with `ref`
    // observer name -> the owner it observes (`ref Node b = a;` => b observes a).
    // Using an observer whose owner is moved-from is a dangling use.
    std::map<std::string, std::string> observerOf_;
    // Observers whose observed object is already gone (owner reassigned to a fresh
    // object, or the owner's scope ended). Using them is a dangling use.
    std::set<std::string> danglingObservers_;
    // Observer names created via a summary-derived return alias (`Node r =
    // pick(x, y);` where pick's summary says it returns param 0 back out), as
    // opposed to an explicit `ref` observer. For these, `observerOf_[r]`'s owner
    // being moved-from is EXPECTED (the very call that created the alias moves its
    // argument) and must NOT by itself count as dangling -- the returned object is
    // still alive, just no longer reachable through the original owner's name. Only
    // an explicit death event (reassignment/scope-exit of the owner, tracked via
    // danglingObservers_) makes an alias observer dangling, same as a real observer.
    std::set<std::string> aliasObservers_;
    std::vector<DanglingError> errors_;
    const SmartPointerAnalyzer* analyzer_ = nullptr;  // set for the duration of check()
    const Program* program_ = nullptr;                // set for the duration of check()
    const std::vector<StatementPtr>* currentBody_ = nullptr;  // the body checkBody is walking
    std::set<std::string> currentParamNames_;
};

} // namespace cling
