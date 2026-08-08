#pragma once

#include "../cst_to_ast/ast.h"
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

} // namespace cling
