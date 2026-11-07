#pragma once

#include "syntax/ast.h"

namespace cling {

// Turns every ClosureExpression into an ordinary ClassDeclaration plus a NewExpression, so no
// later pass needs to know closures exist. Runs right after parsing and before EscapeHatchChecker.
//
// `closure IFace [count] (int x) { body }` becomes
//   class __closure_0 : public IFace {
//       int count;
//       __closure_0(int count) { this->count = count; }
//       <IFace's one virtual method> override { body }
//   };
// and the expression becomes `new __closure_0(count)`. The class is inserted before the top-level
// statement that contained the closure.
//
// Closures nest. An inner capture list may name only the enclosing closure's parameters, not its
// captures (those are fields reachable only through `this`, which cannot be captured).
//
// Throws std::runtime_error for:
//   - a captured name not in scope at the closure (this also rejects an inner closure capturing an
//     outer capture)
//   - a captured `ref` observer (it would be copied into a field that outlives it)
//   - capturing `this`
//   - an interface name that is not a declared class
//   - an interface with zero or several virtual methods
//   - a parameter list that does not match the interface method
void desugarClosures(Program& program);

// True if `type` is a plain value that may be copied into a spawned thread: a primitive (incl. string), a
// std::vector of such values, or a `struct` made only of such values (base struct included). Used by the
// desugar pass for capture checks and by DanglingChecker for the fields of a moved-in object.
bool isPlainValueType(const std::string& type, const Program& program);

} // namespace cling
