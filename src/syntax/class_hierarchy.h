#pragma once

#include "syntax/ast.h"
#include <string>
#include <vector>

namespace cling {

// Shared inheritance helpers (single inheritance; see grammar.js's class_declaration).
//
// Every analysis module -- DanglingChecker, EscapeHatchChecker, AstEmitter,
// SmartPointerAnalyzer -- keeps its own private, FLAT `className -> members` map,
// built by its own collection pre-pass, and then answers ~40 "does class X have
// member Y" questions against it with an exact lookup. Rather than teach all 40 of
// those sites to walk a base chain, each collection pass instead visits classes in
// base-before-derived order and copies a base's already-flattened members down into
// its derived classes. Every downstream lookup then works unchanged.
//
// That only works if the visit order is genuinely base-before-derived, which is what
// this helper provides -- computed once here instead of being re-derived (and
// re-bugged) in four separate places.

// Returns every ClassDeclaration in `program` ordered so that a base class always
// appears before any class deriving from it.
//
// Assumes the inheritance graph is acyclic and every base names a declared class --
// both guaranteed by ASTBuilder::validateInheritance, which runs at parse time,
// before any analysis module sees the Program. A class whose base is missing (which
// cannot happen post-validation) is simply emitted in input order rather than
// dropped, so this can never silently lose a class.
std::vector<const ClassDeclaration*> classesInBaseFirstOrder(const Program& program);

// The internal lookup key a method SPECIALIZATION is registered/resolved under
// (`specializationKey("as", "int")` -> `"as<int>"`), reused verbatim by every
// analysis module's classMethods_/classMethodParams_-style maps -- no new data
// structures needed, just a naming convention every existing per-method lookup
// site already keys its maps by. A specialization (`int as<int>() {...}`) is
// registered under this key, NOT under the bare method name; a call carrying a
// type argument (`n.as<int>()`, FunctionCall::templateArg == "int") is resolved
// by constructing this same key instead of using the bare method name. The
// primary generic (`<T> T as() {...}`) is registered under the bare name as
// usual, since it is a real (if never meaningfully callable) method.
inline std::string specializationKey(const std::string& methodName, const std::string& typeArg) {
    return methodName + "<" + typeArg + ">";
}

// The internal lookup key for one overload of an OVERLOADED method (same name,
// different parameter list) -- `overloadKey("operator[]", 1)` -> "operator[]/1".
// Reused verbatim by every module's classMethods_/classMethodParams_-style maps,
// same "no new data structures" reasoning as specializationKey. Only classes that
// genuinely declare more than one method of the same name need this: collection
// passes key by PLAIN bare name as before for the common non-overloaded case, and
// only switch to this key when a real name collision is detected, keeping the
// common case's key shape (and therefore every other lookup site) unchanged.
//
// Deliberately keyed by PARAMETER COUNT only, not full parameter types: distinct
// argument counts is enough to disambiguate CeeLing's actual overload shapes
// (e.g. yaml-cpp-style `operator[](string)` vs `operator[](int)`, both arity 1,
// still collide under this key -- call-site resolution for THOSE needs the
// call's own argument shape, not just count, and is intentionally not attempted
// here; real C++ overload resolution already picks the right body correctly at
// the call site regardless, so this key only protects the SEPARATE concern of a
// checker/emitter module's internal move/pointer-kind bookkeeping from
// cross-contaminating between overloads of different arity).
inline std::string overloadKey(const std::string& methodName, size_t paramCount) {
    return methodName + "/" + std::to_string(paramCount);
}

} // namespace cling
