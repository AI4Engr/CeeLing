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

// Phase 1 (guide chapter 13 threat model #13-18): seal the escape hatches that would let a
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
// Also enforces the design notes' "default-reject unknown library calls" rule: a call whose
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

    // If `expr` is DIRECTLY the bare Identifier `this` (not merely containing
    // `this` somewhere inside a larger expression -- e.g. NOT `this.foo()`'s
    // receiver) and we're currently inside a constructor, record a rejection
    // at `anchor`'s source location. Shared by the three positions where
    // AstEmitter::emitMovedArg would otherwise lower a bare `this` to
    // `shared_from_this()`: a call argument, a VariableDeclaration RHS, and an
    // AssignmentStatement RHS -- see checkExpression's FunctionCall branch and
    // checkStatement's VariableDeclaration/AssignmentStatement branches.
    void rejectThisEscapeInConstructor(const Expression* expr, const ASTNode* anchor);

    // Resolve the CeeLing class name of `expr`, or "" if it isn't a known object type
    // (unresolved shapes are treated as unknown, not flagged -- this check only needs
    // the direct, common receiver shapes, not full type inference).
    std::string resolveObjectType(const Expression* expr) const;

    std::set<std::string> classNames_;
    std::set<std::string> enumNames_;

    // Unknown type names (2026-11-02): a declaration whose type the compiler has no shape for is an error, not a pass-through.
    // `typeIsKnown` is true for a primitive, `string`, a class/struct/enum of this program, a type some loaded fact pack declares
    // (`class X`), and `std::vector<T>` / `std::map<K,V>` / `Channel<T>` of known types.
    bool typeIsKnown(const std::string& type) const;
    // allowView: a `string_view` is acceptable here (a local variable or a parameter); everywhere else it is refused with its own reason.
    void requireKnownType(const std::string& type, const std::string& what, int line, int column, bool allowView = false);
    std::set<std::string> reportedUnknownTypes_;
    std::string typeParameter_;   // the generic method being checked (`<T> T as()`), whose `T` is not an unknown type
    // className -> (fieldName -> declared type)
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    // Classes that declare their own `get()` method -- a `.get()` call on these is an
    // ordinary user method call, not the smart-pointer back door, so it is not flagged.
    std::set<std::string> classesWithOwnGet_;
    // Free-function names declared at the top level.
    std::set<std::string> knownFreeFunctions_;
    // Free-function name -> declared return type. Same purpose as
    // classMethodReturnType_, for a chained call whose FIRST link is a plain
    // `f()` call rather than a `recv.method()` call (e.g. `makeHolder().getInner()`).
    std::map<std::string, std::string> freeFunctionReturnType_;
    // className -> set of method names declared on that class (constructors excluded
    // -- they are never called by name, only via `new T(...)`, a distinct AST node).
    std::map<std::string, std::set<std::string>> classMethods_;
    // className -> (methodName -> declared return type). Populated alongside
    // classMethods_ in the same base-first loop (inherited the same way). Lets
    // resolveObjectType resolve a CHAINED call receiver (`a.b().c()`) by looking
    // up what `b()` returns instead of giving up the moment the receiver isn't a
    // bare Identifier -- see resolveObjectType's own comment for why this was
    // previously "not resolved by this lightweight pass" and why it's safe to
    // widen now (CeeLing objects are always smart-pointer-wrapped, so a method's
    // return value is a live reference-counted/owning copy, never a dangling
    // alias into a temporary that's already been destroyed).
    std::map<std::string, std::map<std::string, std::string>> classMethodReturnType_;
    // "Class::as<int>"-style key (className + "::" + specializationKey) -> every
    // arity (parameter count) a specialization under that key is actually
    // declared with. ALWAYS populated for every specialization (not just ones
    // that collide with a same-type-argument sibling), so a call's own argument
    // count can be checked against real declared arities regardless of whether
    // more than one exists -- see isKnownCallable's templateArg branch. Needed
    // because `classMethods_`'s specializationKey membership alone says nothing
    // about arity (confirmed via probe: yaml-cpp's `as<T>()`/`as<T>(T fallback)`
    // pair needs two distinct C++ template primaries with different parameter
    // lists, so a call to a nonexistent arity must be rejected, not silently
    // let through to fail at the g++ stage instead).
    std::map<std::string, std::set<size_t>> classSpecializationArities_;
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
