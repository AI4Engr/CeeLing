#pragma once

#include "../cst_to_ast/ast.h"
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
// Not yet handled: `ref` observers, scope-exit deaths, reassignment revival across
// branches, library summaries. Those arrive in later Phase 2 increments.
//
// Conservative by construction: it only flags a use that provably follows a move on
// the linear statement order within one function body. It never rewrites code and has
// no false-positive path for the constructs it does not model (they are simply
// ignored, not rejected — tightening comes later).
class DanglingChecker {
public:
    // Returns all detected errors (empty => program is accepted by this checker).
    std::vector<DanglingError> check(const Program& program);

private:
    void collectClassNames(const Program& program);

    // Analyze one function/method body: clears per-body state, then processes each
    // statement in order.
    void checkBody(const std::vector<StatementPtr>& body);

    // Process one statement in the CURRENT scope (does not clear state). Nested blocks
    // (e.g. an `if` body) reuse the enclosing scope so outer observers stay visible;
    // owners declared inside the block die when the block ends.
    void processStatement(const Statement* stmt);

    // Owners declared at the end of a block die: mark their observers dangling and drop
    // them from the live set. `ownersBefore` is the object-local set captured on entry.
    void endBlockScope(const std::set<std::string>& ownersBefore);

    // Record which locals are object-typed (only those participate in move tracking).
    // Returns true if `type` names a known class.
    bool isObjectType(const std::string& type) const;

    // Walk an expression; for every use of a moved-from variable, emit an error.
    void checkExpressionUses(const Expression* expr);

    // If `expr` is a bare object-typed local, return its name; else empty.
    std::string asObjectLocalName(const Expression* expr) const;

    std::set<std::string> classNames_;
    std::set<std::string> objectLocals_;   // object-typed owners live in current body
    std::set<std::string> movedFrom_;      // moved-from locals in the current body
    std::set<std::string> observerNames_;  // all names declared with `ref`
    // observer name -> the owner it observes (`ref Node b = a;` => b observes a).
    // Using an observer whose owner is moved-from is a dangling use.
    std::map<std::string, std::string> observerOf_;
    // Observers whose observed object is already gone (owner reassigned to a fresh
    // object, or the owner's scope ended). Using them is a dangling use.
    std::set<std::string> danglingObservers_;
    std::vector<DanglingError> errors_;
};

} // namespace cling
