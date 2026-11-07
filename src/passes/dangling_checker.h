#pragma once

#include "syntax/ast.h"
#include "passes/smart_pointer_analyzer.h"
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
    // The `semantic/<code>` category printed with the error.
    std::string code = "use-after-move";
};

// Dangling-reference and use-after-move checker. It follows owners, observers (`ref`,
// views, spans, returned aliases) and moves through one function body at a time, in
// statement order, with a conservative merge at branches and loops. It never rewrites code.
// Object-typed parameters are always moved into the callee (there are no `ref` parameters),
// so a function summary only records whether the return value aliases one of its parameters.
class DanglingChecker {
public:
    // Returns all detected errors (empty means accepted). `analyzer` must already have run
    // over the same `program`: whether a `push_back` argument is shared or moved depends on
    // the pointer kind the analyzer chose for it.
    std::vector<DanglingError> check(const Program& program, const SmartPointerAnalyzer& analyzer);

private:
    // Per-function/method borrow contract, derived from the body.
    struct FunctionSummary {
        // Index of the parameter the return value aliases, or -1 if it is always fresh
        // (or the function does not return an object).
        int returnBorrowsParam = -1;
    };

    void collectClassNames(const Program& program);

    // Computes a FunctionSummary for every function and method before any body is checked,
    // so forward references already have one.
    void deriveSummaries(const Program& program);
    FunctionSummary deriveSummaryForBody(
        const std::vector<StatementPtr>& body,
        const std::vector<std::pair<std::string, std::string>>& params) const;

    // Qualified summary key: free functions by name, methods by "ClassName::name".
    static std::string summaryKey(const std::string& ownerClass, const std::string& name) {
        return ownerClass.empty() ? name : ownerClass + "::" + name;
    }

    // Analyzes one function/method body: clears per-body state, registers the parameters,
    // then processes each statement in order. `ownerClass` is "" for free functions.
    void checkBody(const std::vector<StatementPtr>& body,
                    const std::vector<std::pair<std::string, std::string>>& params,
                    const std::string& ownerClass, const std::string& scope);

    // Processes one statement in the current scope. Nested blocks reuse the enclosing
    // scope; owners declared inside a block die when it ends.
    void processStatement(const Statement* stmt);

    // Owners declared in a block die at its end: their observers become dangling.
    // `ownersBefore` is the object-local set captured on entry.
    void endBlockScope(const std::set<std::string>& ownersBefore, const std::set<std::string>* stringsBefore = nullptr);

    // A `string_view` is an observer of the string local or parameter it was made from.
    // Reassigning the string, a mutating method on it or the end of its scope kills the view.
    // A view of a literal or of a view parameter has no owner here. Anything else (a call
    // result, a field, a concatenation) is refused.
    std::set<std::string> stringOwners_;   // string locals and parameters currently in scope
    void bindStringView(const std::string& name, const Expression* init, const Statement* at);
    // A `span<T>` is the same kind of observer, of a vector local or parameter: the vector's
    // reassignment, a method that may reallocate it, its being moved away and the end of its
    // scope kill the span. It can be made from such a vector, another span, or first/last/subspan
    // of a span; anything else is refused.
    void bindSpan(const std::string& name, const Expression* init, const Statement* at);

    // The state a branch body can change that must not leak into the sibling branch.
    struct BranchState {
        std::set<std::string> movedFrom;
        std::set<std::string> observerNames;
        std::map<std::string, std::string> observerOf;
        std::set<std::string> danglingObservers;
        std::set<std::string> aliasObservers;
    };
    BranchState snapshotBranchState() const;
    void restoreBranchState(const BranchState& state);
    // Conservative union of the two branch outcomes after an `if`/`else`: an effect in either
    // branch is kept, and an observer the branches disagree about is dangling.
    void mergeBranchStates(const BranchState& before, const BranchState& thenState,
                            const BranchState& elseState);

    // One entry per `while`/`for` being analyzed (innermost last). A `break` or `continue`
    // leaves the straight-line flow, so its effects must not reach the statements after it in
    // the body, but they do reach the code after the loop. Each records its state here and the
    // loop merges them into its post-loop state.
    struct LoopContext {
        std::set<std::string> ownersBefore;     // object locals declared before the loop
        std::vector<BranchState> breakExits;    // state at each `break`: leaves the loop only
        std::vector<BranchState> continueExits; // state at each `continue`: next iteration or exit
    };
    std::vector<LoopContext> loopStack_;
    // Records the state at a break/continue (with the loop's own scope still open).
    void recordLoopExit(bool isContinue);
    // Analyzes a `while` (condition != nullptr) or range-`for` (forStmt != nullptr) body. The body
    // is analyzed again from the state the previous pass left at the loop head until that state
    // stops changing, so a move in one iteration is seen by the next. The state after the loop is
    // the union of: no iteration ran, every pass's end, every break and every continue.
    void processLoop(const Expression* condition, const ForStatement* forStmt,
                     const std::vector<StatementPtr>& body);

    // True if `type` names a known class or a recognized container type. Only these take part
    // in move and observer tracking.
    bool isObjectType(const std::string& type) const;
    // True if `name` is an object local whose pointer kind resolved to shared: assigning it
    // onward copies the handle, so it is not consumed.
    bool isSharedLocal(const std::string& name) const;

    // True if `expr` is provably a string value: a literal, a string-typed identifier, or a `+`
    // of such operands. Used to restrict `throw` payloads to strings.
    bool isStringValuedExpr(const Expression* expr) const;

    // Walks an expression and reports every use of a moved-from variable.
    void checkExpressionUses(const Expression* expr);
    // For opaque external classes (`opaque X` in a fact pack): a call to a method with no fact
    // kills every observer of its receiver. Walks the whole expression before any use in it is
    // checked, because operand evaluation order is unspecified (kill first).
    void killObserversOfOpaqueCalls(const Expression* expr);
    // `Grid w = g.merge(h);` with `merge` unlisted on an opaque class: the result may borrow `h`
    // as well as `g` and an observer follows one owner, so it is refused.
    void rejectOpaqueResultWithObjectArgs(const Expression* init, const Statement* at);
    // `Grid w = field.window(..)`: the result borrows a field, which has no owner this analysis
    // follows, so it is refused.
    void rejectBorrowFromFieldReceiver(const Expression* init, const Statement* at);
    int exprDepth_ = 0;

    // The name of `expr` if it is a bare object-typed local, else "".
    std::string asObjectLocalName(const Expression* expr) const;

    // If `expr` is `v[i]` on a container local, the container's name (an element has no name;
    // provenance is tracked per container), else "".
    std::string asContainerSubscript(const Expression* expr) const;

    // The owner a `ref` or alias bound to `expr` observes: asObjectLocalName for an identifier,
    // asContainerSubscript for `v[i]`.
    std::string resolveObservedOwner(const Expression* expr) const;

    // Binding a `ref` is sound only when the owner is trackable: an object local, `v[i]` of a
    // container local, another observer, or `nullptr`. A field path or call result is rejected.
    // On success `owner` is the tracked owner ("" for nullptr or an observer with none) and
    // `inheritsDangling` says the source observer was already dangling.
    bool resolveRefBinding(const Expression* expr, std::string& owner,
                           bool& inheritsDangling) const;
    void reportUntrackableRefBinding(const std::string& name, const Expression* expr,
                                     const ASTNode* anchor);

    // True if `assign` is `n = value;` with `n` of a class that declares its own `operator=`
    // and `value` not an object of that class: a mutating call, not an ownership reassignment.
    bool isOperatorAssignMutation(const AssignmentStatement& assign) const;

    // A call that may reallocate its container receiver kills every existing observer of it.
    void invalidateContainerObservers(const Expression* callExpr);
    // A span of a field vector of the current class (`std::span<int> s = items;` in a method)
    // observes the pseudo owner "@items". Nothing follows a field across calls, so such a span
    // dies at an assignment to the field, a vector mutator called on it, and any call that may
    // reach the heap (a user function that is not heap-pure, or a `new`). Passing such a span to
    // a call that may change the field is refused.
    void killFieldObservers(const std::string& field);      // "" = every field observer
    bool hasFieldObservers() const;
    // 0: not user code (a library call), 1: every user function/method of that name is heap-pure, 2: some is not
    int calleeHeapEffect(const FunctionCall* call) const;

    // The class name of `expr`'s static type: a bare identifier (local, parameter or field) or
    // a chained call (`a.b()`, `f()`). Returns "" when unresolved.
    std::string resolveClassOfExpr(const Expression* expr) const;

    // If `expr` calls a known function whose summary says the result aliases a parameter,
    // the name of the object-local argument at that position, else "".
    std::string resolveCallReturnAlias(const Expression* expr) const;

    // If `expr` is `receiver.method(args)` on a known external class and `method` is an
    // allow-listed borrowing method, the receiver's name, else "". Unlike an aliased call
    // argument, the receiver is not moved by the call, so callers must not add the resulting
    // observer to aliasObservers_.
    std::string resolveBorrowingMethodReceiver(const Expression* expr) const;

    // Applies the moves and invalidations of every call inside `expr`, in evaluation order
    // (operands and arguments before the call that takes them), however deep. A call that moves
    // an owner the same statement already moved is a use of a moved-from value.
    void applyCallArgMoves(const Expression* expr);
    void applyOneCallMoves(const Expression* expr);      // the single call `expr`

    // `cling::spawn(new __closure_N(captures...))`: decides which object captures may be moved
    // into the new thread and marks each one moved-from; rejects the rest. Rules are in the .cpp.
    void checkSpawnCall(const FunctionCall* call);
    // The reason object `name` of class `type` may not be moved into a thread, or "" if it may.
    std::string spawnObjectProblem(const std::string& type, const std::string& name) const;

    // Rejects `return b;` when `b` is a `ref` observer declared in the current body: it would
    // refer into the returning function's own frame.
    void checkReturnedReference(const Expression* expr);

    std::set<std::string> classNames_;
    // className -> its field names. Lets a bare `link = other;` in a method be recognized as a
    // field store rather than a local reassignment.
    std::map<std::string, std::set<std::string>> classFields_;
    // className -> field name -> declared type text.
    std::map<std::string, std::map<std::string, std::string>> classFieldTypes_;
    // The declared type of a receiver expression (local, parameter, field of the current class,
    // or `obj.field` of a known class); "" if unknown.
    std::string declaredTypeOf(const Expression* expr) const;
    // summaryKey() -> derived summary.
    std::map<std::string, FunctionSummary> summaries_;
    // Declared parameter lists, used to resolve a call's aliased argument by index.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> functionParams_;
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, std::string>>>> classMethodParams_;
    // Declared return types, used by resolveClassOfExpr to resolve a chained receiver.
    std::map<std::string, std::string> functionReturnType_;
    std::map<std::string, std::map<std::string, std::string>> classMethodReturnType_;

    // (class, method) -> index of the parameter that is a mutable out-parameter and is not
    // moved into the callee. Set for a method whose primary generic uses its type parameter T
    // as a parameter type (`<T> bool decodeInto(T out)`); every specialization shares it.
    std::map<std::string, size_t> outParamIndex_;
    static std::string outParamKey(const std::string& ownerClass, const std::string& methodName) {
        return ownerClass + "::" + methodName;
    }
    // ext_fn name -> indices of its `ref` parameters. Such an argument is borrowed for the call,
    // not moved, like outParamIndex_.
    std::map<std::string, std::set<size_t>> externRefParams_;

    std::string currentClass_;  // enclosing class of the body being checked ("" = free fn)
    std::string currentScope_;  // analyzer scope string of the body being checked
    std::set<std::string> objectLocals_;   // object-typed owners live in current body
    std::map<std::string, std::string> localType_;  // object-local/param name -> class name
    std::set<std::string> movedFrom_;      // moved-from locals in the current body
    std::set<std::string> observerNames_;  // all names declared with `ref`
    // observer name -> the owner it observes. Using an observer whose owner is moved-from is
    // a dangling use.
    std::map<std::string, std::string> observerOf_;
    // Observers whose object is already gone (owner reassigned or out of scope).
    std::set<std::string> danglingObservers_;
    // Names bound to a call result that aliases an argument (`Node r = pick(x, y);`). The call
    // moved the argument, so the owner being moved-from does not make these dangle; only an
    // explicit death event of the owner does.
    std::set<std::string> aliasObservers_;
    std::vector<DanglingError> errors_;
    const SmartPointerAnalyzer* analyzer_ = nullptr;  // set for the duration of check()
    const Program* program_ = nullptr;                // set for the duration of check()
    const std::vector<StatementPtr>* currentBody_ = nullptr;  // the body checkBody is walking
    std::set<std::string> currentParamNames_;
};

} // namespace cling
