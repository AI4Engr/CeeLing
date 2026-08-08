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
};

// Minimal single-pass use-after-move / dangling checker (Phase 2 seed).
//
// Scope of THIS version (deliberately small — the first fixture to light up):
//   * "move" events: passing an object-typed local as a function-call argument, and
//     assigning one object-typed local from another (`Node b = a;` moves a).
//   * "use" check: any later use of a moved-from variable is an error.
// Auto-derived return-provenance summaries (CLAUDE.md component A) are also
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
    // Auto-derived per-function/method borrow contract (CLAUDE.md component A).
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
                    const std::string& ownerClass);

    // Process one statement in the CURRENT scope (does not clear state). Nested blocks
    // (e.g. an `if` body) reuse the enclosing scope so outer observers stay visible;
    // owners declared inside the block die when the block ends.
    void processStatement(const Statement* stmt);

    // Owners declared at the end of a block die: mark their observers dangling and drop
    // them from the live set. `ownersBefore` is the object-local set captured on entry.
    void endBlockScope(const std::set<std::string>& ownersBefore);

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

    // Record which locals are object-typed (only those participate in move tracking).
    // Returns true if `type` names a known class OR a recognized container type
    // (e.g. `std::vector<Node>` -- see library_summary.h). A container's own
    // lifetime matters exactly like a class object's (moving it moves the backing
    // store; an observer of a moved-from/killed container dangles), so it reuses
    // 100% of the existing move/observer machinery once recognized here.
    bool isObjectType(const std::string& type) const;

    // Walk an expression; for every use of a moved-from variable, emit an error.
    void checkExpressionUses(const Expression* expr);

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

    // A known invalidating library call (only `push_back` this slice -- see
    // library_summary.h) kills every existing observer of its container receiver:
    // the backing store may have reallocated, so any `ref`/subscript-derived
    // reference into the old storage is now dangling. Structurally identical to
    // what endBlockScope already does for scope-exit and the AssignmentStatement
    // branch already does for reassignment -- just a third death-event source,
    // reusing the same danglingObservers_ set (already conservatively unioned
    // across branches by mergeBranchStates, so this needs no new BranchState field).
    void invalidateContainerObservers(const Expression* callExpr);

    // If `expr` is a call to a known CeeLing function/method whose summary says the
    // return value aliases one of its parameters, return the NAME of the object-local
    // argument passed at that position (empty if not applicable: unknown callee,
    // summary says "fresh", or the aliased argument isn't a bare object local).
    std::string resolveCallReturnAlias(const Expression* expr) const;

    // If `expr` is a call, every object-typed-local argument is moved into the
    // callee (CeeLing bans `ref` parameters, so a call always consumes its object
    // arguments). No-op for non-call expressions.
    void applyCallArgMoves(const Expression* expr);

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
    // Qualified summary key (summaryKey()) -> derived summary.
    std::map<std::string, FunctionSummary> summaries_;
    // Free-function name -> declared parameter list (needed to resolve a call's
    // aliased-argument expression by index). Methods are looked up per-receiver-class
    // via classMethodParams_.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> functionParams_;
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, std::string>>>> classMethodParams_;

    std::string currentClass_;  // enclosing class of the body being checked ("" = free fn)
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
};

} // namespace cling
