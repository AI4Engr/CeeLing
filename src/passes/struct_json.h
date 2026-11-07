#pragma once

#include "syntax/ast.h"

namespace cling {

// Generates JSON-decoding functions for `struct`s, on request by name.
//
// A call to a free function `decodeX(item)` where `X` is a `struct` declared in THIS file and nobody
// declared a function called `decodeX` makes the compiler write it:
//
//     struct Args { string root; int depth; std::vector<string> names; Args(string r, int d, std::vector<string> n) {...} };
//     Args a = decodeArgs(item);
//
// becomes (conceptually)
//
//     Args decodeArgsAt(Item j, string path) { ...check each field, throw "invalid_argument: `root` must be a string"... }
//     Args decodeArgs(Item j) { return decodeArgsAt(j, ""); }
//
// Field types: int, int64, double, float, bool, string, byte, std::vector of those, a `struct` (decoded by its own
// `decodeNAt`, generated too when the struct is in this file, or already declared), std::vector of a `struct`.
// The struct needs a constructor whose parameter types are its field types in declaration order; the generated code
// decodes every field into a local and ends with `return new X(v0, v1, ...)`. A missing field or a field of the wrong
// type throws; an extra field in the JSON is ignored. The code uses the cjson-cling names (Item, isObject, isString,
// isNumber, isBool, isTrue, isArray, getObjectItem, getArraySize, getArrayItem), which must be visible
// (`#include "cjson_api.clh"`); a program that lacks them gets an error that says so.
//
// The generated functions are ordinary CeeLing: they are parsed from generated source text and go through the same
// checks as hand-written code. Run before desugarClosures and every checker. Only the file that declares the struct
// generates its decoder (a call from another file needs a function declared there, or a one-line wrapper).
//
// Throws std::runtime_error with the compiler's usual "error[semantic/...]" text.
//
// `included` is true for a file merged into another one through `.clh`: the pass writes its decoders so they are declared for the includer,
// but does not demand cjson-cling here, because a diamond include may have merged it into a sibling instead (the file that emits the definition is
// compiled on its own and checked there).
void desugarStructJson(Program& program, bool included = false);

}  // namespace cling
