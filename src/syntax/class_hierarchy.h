#pragma once

#include "syntax/ast.h"
#include <string>
#include <vector>

namespace cling {

// Inheritance helpers (single inheritance). Each analysis pass keeps a flat
// `className -> members` map and copies a base's members into its derived classes,
// which needs classes visited base before derived.

// Every ClassDeclaration in `program`, a base class always before its derived classes.
// Assumes the graph is acyclic and every base is declared (ASTBuilder::validateInheritance
// checks this); a class with a missing base is kept in input order, never dropped.
std::vector<const ClassDeclaration*> classesInBaseFirstOrder(const Program& program);

// Lookup key of a method specialization: specializationKey("as", "int") is "as<int>".
// A specialization (`int as<int>() {...}`) is registered under this key, not the bare
// name; a call with a type argument (`n.as<int>()`) is resolved with the same key.
inline std::string specializationKey(const std::string& methodName, const std::string& typeArg) {
    return methodName + "<" + typeArg + ">";
}

// Lookup key of one overload of an overloaded method: overloadKey("operator[]", 1) is
// "operator[]/1". Used only when a class declares two methods of the same name; other
// methods keep the bare name. Keyed by parameter count only, so overloads of the same
// arity (`operator[](string)` and `operator[](int)`) still share a key; C++ overload
// resolution picks the right body anyway, the key only keeps per-method bookkeeping of
// different arities apart.
inline std::string overloadKey(const std::string& methodName, size_t paramCount) {
    return methodName + "/" + std::to_string(paramCount);
}

} // namespace cling
