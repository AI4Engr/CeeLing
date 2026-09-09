#pragma once

#include "../cst_to_ast/ast.h"

namespace cling {

// Desugars every ClosureExpression in the program into an ordinary
// ClassDeclaration + NewExpression pair, so no checker (EscapeHatchChecker,
// SmartPointerAnalyzer, DanglingChecker) or the emitter ever needs to know
// closures exist. Run once, immediately after ASTBuilder::buildProgram (and
// its own spliceExternalSpecializations) and BEFORE EscapeHatchChecker --
// EscapeHatchChecker hard-rejects any construct it doesn't recognize, so a
// ClosureExpression that reached it would be rejected as unknown, not
// analyzed as the class it's about to become.
//
// `closure IFace [count] (int x) { body }` becomes:
//   class __closure_0 : public IFace {
//   public:
//       int count;
//       __closure_0(int count) { this->count = count; }
//       <IFace's one virtual method's signature> override { body }
//   };
// with the closure expression itself replaced by `new __closure_0(count)`.
// The synthesized ClassDeclaration is inserted into Program::statements
// immediately before the top-level statement that contained the closure.
//
// NESTING: a closure literal may appear inside another closure's own body,
// to unbounded depth. An inner closure's capture list may only name its
// IMMEDIATELY ENCLOSING closure's own PARAMETERS -- never that closure's own
// CAPTURES. An outer capture is a FIELD of the outer's synthesized class,
// reachable only through the outer's own `this`, and closures cannot capture
// `this` (see the rejection list below) -- so this restriction falls out of
// the existing capture-resolution rejection with no separate mechanism: the
// inner closure's scope simply never contains the outer's captured names.
//
// Throws std::runtime_error (matching EscapeHatchChecker/ASTBuilder's own
// convention -- see rejectObserverParams) for every case CLAUDE.md's
// "default-reject, never default-allow" rule requires closing off:
//   - a captured name that isn't in scope at the closure's own position
//     (this is also what rejects a nested closure trying to capture its
//     enclosing closure's own CAPTURES, per the nesting rule above)
//   - a captured name that names a `ref` observer (would copy a non-owning
//     reference into a field that outlives it -- the most important check
//     here, since this is exactly the dangling-reference hole closures could
//     otherwise reopen)
//   - capturing `this`
//   - an interface name that isn't a declared class
//   - an interface with zero or more than one virtual method
//   - a closure whose parameter list doesn't match the interface method's
void desugarClosures(Program& program);

} // namespace cling
