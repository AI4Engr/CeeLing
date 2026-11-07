#pragma once

#include "syntax/ast.h"

namespace cling {

// Generates JSON-decoding functions for `struct`s, on request by name.
//
// A call to a free function `decodeX(item)`, where `X` is a `struct` declared in this file and no
// function `decodeX` exists, makes the compiler write it: `decodeXAt(Item j, string path)` checks
// every field and throws `invalid_argument: ...` on a missing or mistyped one, and `decodeX(Item j)`
// calls it with an empty path.
//
// Field types: int, int64, double, float, bool, string, byte, std::vector of those, a `struct`,
// std::vector of a `struct`. The struct needs a constructor taking its fields in declaration order;
// the decoder ends in `return new X(v0, v1, ...)`. Extra JSON keys are ignored. The code uses the
// cjson-cling names (Item, isObject, getObjectItem, ...), which must be visible through
// `#include "cjson_api.clh"`.
//
// The generated functions are parsed from generated source text and checked like hand-written code.
// Runs before desugarClosures and every checker. Only the file that declares the struct generates
// its decoder.
//
// Throws std::runtime_error with the usual "error[semantic/...]" text.
//
// `included` is true for a file merged into another through `.clh`: the decoders are written so the
// includer sees their declarations, but cjson-cling is not demanded here (a diamond include may
// have merged it into a sibling).
void desugarStructJson(Program& program, bool included = false);

}  // namespace cling
