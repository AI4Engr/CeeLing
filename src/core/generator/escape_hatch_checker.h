#pragma once

#include "../cst_to_ast/ast.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cling {

// A single escape-hatch diagnostic, anchored to a .clg source line.
struct EscapeHatchError {
    int line = 0;
    int column = 0;
    std::string message;
    std::string help;
};

// Phase 1 (CLAUDE.md threat model #13-18): seal the escape hatches that would let a
// user route around the smart-pointer/dangling analysis entirely. Most hatches (raw
// pointer types, reinterpret_cast/const_cast, raw union, raw delete) have no grammar
// production at all, so they already fail to parse; this checker covers the one hatch
// that DOES parse cleanly today: `.get()` on a compiler-generated smart pointer.
//
// `.get()` is only rejected when the receiver resolves to a CeeLing object type (i.e.
// the compiler will emit it as unique_ptr/shared_ptr/weak_ptr under the hood) AND the
// receiver's class does not itself declare a `get()` method. A user's own class may
// legally define its own `get()` (e.g. `Box.get()`); that is an ordinary method call,
// not the smart-pointer back door, and is not touched by this checker.
//
// Also enforces CLAUDE.md's "default-reject unknown library calls" rule: a call whose
// target is not a CeeLing-defined free function or method is rejected outright. Today
// CeeLing has no mechanism to learn a real signature for anything reached via
// `#include` (include lines are passed through as opaque text, never parsed for
// declarations), so "unknown call" and "call to something outside this program" are
// currently the same set -- there is no legitimate third category yet. This closes
// the gap where `Node r = someLibraryFn(a, b);` was silently treated as "a fresh
// owner" with zero knowledge of what the callee actually does to its arguments or
// what its return value aliases (see DanglingChecker's auto-derived summaries, which
// this call would have no summary for at all). `std::cout << ...` stream output is a
// distinct `BinaryExpression`/`ScopedIdentifier` shape, never a FunctionCall node, so
// it is structurally exempt without any special-casing here.
class EscapeHatchChecker {
public:
    // Returns all detected errors (empty => program has no Phase-1 violations).
    std::vector<EscapeHatchError> check(const Program& program);

private:
    void collectClasses(const Program& program);
    // Populate knownFreeFunctions_ / classMethods_ from every top-level
    // FunctionDeclaration / class method, so call targets can be resolved.
    void collectCallables(const Program& program);

    // Is `call` a resolvable call to a known CeeLing function/method, OR a known
    // library method on a recognized container type (e.g. `v.push_back(x)` on a
    // std::vector<T> local -- see library_summary.h)? Handles both
    // `recv.method(...)` (receiver's class/container type must be resolvable) and a
    // plain `f(...)` (either a free function, OR -- inside a method body -- a bare
    // same-class method call with an implicit `this` receiver).
    bool isKnownCallable(const FunctionCall& call) const;

    // Declared type string of `expr` if it's a bare local/param/field (unfiltered --
    // unlike resolveObjectType, this does NOT require the type to be a CeeLing
    // class; used to recognize container-typed receivers like `std::vector<Node>`).
    std::string resolveDeclaredType(const Expression* expr) const;

    // Analyze one function/method/constructor body: resets per-body local types.
    // `isConstructor` enables the virtual-call-from-constructor check for this body.
    void checkBody(const std::vector<StatementPtr>& body,
                    const std::vector<std::pair<std::string, std::string>>& params,
                    const std::string& ownerClass, bool isConstructor = false);

    void checkStatement(const Statement* stmt);
    void checkExpression(const Expression* expr);

    // Resolve the CeeLing class name of `expr`, or "" if it isn't a known object type
    // (unresolved shapes are treated as unknown, not flagged -- this check only needs
    // the direct, common receiver shapes, not full type inference).
    std::string resolveObjectType(const Expression* expr) const;

    std::set<std::string> classNames_;
    // className -> (fieldName -> declared type)
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    // Classes that declare their own `get()` method -- a `.get()` call on these is an
    // ordinary user method call, not the smart-pointer back door, so it is not flagged.
    std::set<std::string> classesWithOwnGet_;
    // Free-function names declared at the top level.
    std::set<std::string> knownFreeFunctions_;
    // className -> set of method names declared on that class (constructors excluded
    // -- they are never called by name, only via `new T(...)`, a distinct AST node).
    std::map<std::string, std::set<std::string>> classMethods_;
    // className -> method names that are `virtual` anywhere in that class's
    // hierarchy (flattened like classMethods_, and a method marked `override` counts
    // too -- it is virtual by definition even without repeating the keyword).
    // Calling one of these from a constructor does NOT dispatch to the derived
    // override in C++, so it is rejected; see checkBody's inConstructor_ handling.
    std::map<std::string, std::set<std::string>> classVirtualMethods_;
    // True while checking a constructor body (the only place the virtual-call trap
    // applies -- CeeLing has no destructor syntax, so there are no destructor bodies).
    bool inConstructor_ = false;

    // Per-body state (reset in checkBody).
    std::map<std::string, std::string> localType_;  // local/param name -> declared type
    std::string currentClass_;                       // enclosing class, "" for free fns

    std::vector<EscapeHatchError> errors_;
};

} // namespace cling
