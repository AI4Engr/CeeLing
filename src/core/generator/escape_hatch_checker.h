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
class EscapeHatchChecker {
public:
    // Returns all detected errors (empty => program has no Phase-1 violations).
    std::vector<EscapeHatchError> check(const Program& program);

private:
    void collectClasses(const Program& program);

    // Analyze one function/method/constructor body: resets per-body local types.
    void checkBody(const std::vector<StatementPtr>& body,
                    const std::vector<std::pair<std::string, std::string>>& params,
                    const std::string& ownerClass);

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

    // Per-body state (reset in checkBody).
    std::map<std::string, std::string> localType_;  // local/param name -> declared type
    std::string currentClass_;                       // enclosing class, "" for free fns

    std::vector<EscapeHatchError> errors_;
};

} // namespace cling
