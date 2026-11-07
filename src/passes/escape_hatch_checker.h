#pragma once

#include "syntax/ast.h"
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

// Seals the escape hatches that would let a program route around the smart-pointer and dangling
// analysis. Raw pointers, casts, raw unions and raw delete have no grammar production, so they
// already fail to parse; this checker covers what does parse: `.get()` on a smart pointer, calls to
// anything that is not a CeeLing function or a known library method, unknown type names, and the
// other constructs listed in the .cpp.
//
// `.get()` is rejected only when the receiver is a CeeLing object type whose class does not declare
// its own `get()`. A user-defined `get()` is an ordinary method call.
//
// A call whose target is not a CeeLing free function or method (or a known library method) is
// rejected: the checker has no summary for it, so it cannot know what the callee does to its
// arguments or what the result aliases. Stream output (`std::cout << ...`) is not a FunctionCall
// node, so it needs no special case.
class EscapeHatchChecker {
public:
    // Returns all detected errors (empty means no violations).
    std::vector<EscapeHatchError> check(const Program& program);

private:
    void collectClasses(const Program& program);
    // Fills knownFreeFunctions_ / classMethods_ so call targets can be resolved.
    void collectCallables(const Program& program);

    // Is `call` a call to a known CeeLing function or method, or to a known library method on a
    // recognized container type? Handles `recv.method(...)` and a plain `f(...)` (a free function,
    // or inside a method a same-class method with an implicit `this`).
    bool isKnownCallable(const FunctionCall& call) const;

    // Declared type string of a bare local, parameter or field. Unlike resolveObjectType it does
    // not require a CeeLing class, so container receivers like `std::vector<Node>` resolve.
    std::string resolveDeclaredType(const Expression* expr) const;

    // Checks one function, method or constructor body; resets the per-body local types.
    // `isConstructor` enables the virtual-call-from-constructor check.
    void checkBody(const std::vector<StatementPtr>& body,
                    const std::vector<std::pair<std::string, std::string>>& params,
                    const std::string& ownerClass, bool isConstructor = false);

    void checkStatement(const Statement* stmt);
    void checkExpression(const Expression* expr);

    // If `expr` is exactly the identifier `this` and we are inside a constructor, records a
    // rejection at `anchor`. Covers the three places the emitter would lower a bare `this` to
    // `shared_from_this()`: a call argument and the right side of a declaration or assignment.
    void rejectThisEscapeInConstructor(const Expression* expr, const ASTNode* anchor);

    // Class name of `expr`, or "" if it is not a known object type. Unresolved shapes are treated
    // as unknown, not flagged.
    std::string resolveObjectType(const Expression* expr) const;

    std::set<std::string> classNames_;
    std::set<std::string> enumNames_;

    // True for a primitive, `string`, a class/struct/enum of this program, a type a loaded fact
    // pack declares, and `std::vector<T>` / `std::map<K,V>` / `Channel<T>` of known types.
    bool typeIsKnown(const std::string& type) const;
    // allowView: a `string_view` is acceptable here (a local or a parameter); elsewhere it is
    // refused with its own reason.
    void requireKnownType(const std::string& type, const std::string& what, int line, int column, bool allowView = false);
    std::set<std::string> reportedUnknownTypes_;
    std::string typeParameter_;   // the generic method being checked (`<T> T as()`), whose `T` is not an unknown type
    // className -> (fieldName -> declared type)
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    // Classes that declare their own `get()`; a `.get()` on them is an ordinary method call.
    std::set<std::string> classesWithOwnGet_;
    // Free-function names declared at the top level.
    std::set<std::string> knownFreeFunctions_;
    // Free-function name -> declared return type, for chains that start with `f()`.
    std::map<std::string, std::string> freeFunctionReturnType_;
    // className -> method names declared on that class (inherited ones included, constructors not).
    std::map<std::string, std::set<std::string>> classMethods_;
    // className -> (methodName -> declared return type). Lets resolveObjectType resolve a chained
    // receiver (`a.b().c()`) from what `b()` returns.
    std::map<std::string, std::map<std::string, std::string>> classMethodReturnType_;
    // "Class::as<int>"-style key (className + "::" + specializationKey) -> every parameter count
    // a specialization under that key is declared with, so a call's argument count can be checked.
    std::map<std::string, std::set<size_t>> classSpecializationArities_;
    // className -> method names that are virtual anywhere in the hierarchy (`override` counts).
    // A virtual call from a constructor does not reach the derived override in C++, so it is rejected.
    std::map<std::string, std::set<std::string>> classVirtualMethods_;
    // True while checking a constructor body.
    bool inConstructor_ = false;

    // Per-body state (reset in checkBody).
    std::map<std::string, std::string> localType_;  // local/param name -> declared type
    std::string currentClass_;                       // enclosing class, "" for free fns

    std::vector<EscapeHatchError> errors_;
};

} // namespace cling
