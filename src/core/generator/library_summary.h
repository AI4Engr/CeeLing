#pragma once

#include <string>

namespace cling {

// Hand-written borrow/ownership summaries for a minimal std::vector<T> allow-list
// (Phase 2.6 slice 1). This is the first real entry in CLAUDE.md's library-summary
// allow-list mechanism -- the same one EscapeHatchChecker's default-reject-unknown-
// calls check (Phase 1) demands for anything CeeLing didn't compile itself. Grows
// incrementally; only std::vector<T>'s push_back/size/begin/end are covered here.
// operator[] and iterator-invalidation summaries are deferred to the next slice.

// Is `type` a `std::vector<...>` (or bare `vector<...>`) shape?
bool isVectorType(const std::string& type);

// "std::vector<Node>" -> "Node". Returns "" if `type` isn't a vector<...> shape.
std::string vectorElementType(const std::string& type);

// Is `methodName` a known std::vector<T> method this allow-list covers?
bool isKnownVectorMethod(const std::string& methodName);

// Does calling `methodName` on a vector MOVE its argument at position `argIndex`
// (consume it, the default for any CeeLing call), or does it SHARE (link, like
// storing into a field -- the argument remains a live, usable owner afterward)?
// Only `push_back` is modeled for this slice; everything else defaults to "moves"
// (the safe default) since no other vector method covered here takes an
// object-typed argument.
bool vectorMethodSharesArg(const std::string& methodName, size_t argIndex);

// Does calling `methodName` on a vector potentially reallocate its backing store,
// invalidating every existing reference/iterator into the container (CLAUDE.md
// threat #4)? Only `push_back` is modeled for this slice (the only mutator this
// allow-list covers); `insert`/`resize`/`erase` will extend this set once they
// exist. `size`/`begin`/`end` are read-only and never invalidate.
bool vectorMethodInvalidatesReferences(const std::string& methodName);

// Is `type` CeeLing's `string` primitive (bare keyword, or `std::string`/`string`
// written explicitly)? `string` is a value type -- unlike `std::vector<T>`, it never
// participates in DanglingChecker's move/observer tracking (isObjectType), the same
// way `int`/`bool` don't; this is purely a codegen + allow-list question of which
// name maps to `std::string` and which methods are safe to call on it.
bool isStringType(const std::string& type);

// Is `methodName` a known std::string method this allow-list covers? Deliberately
// excludes `c_str()` (and `data()`): both extract a raw `const char*`, the same
// escape-hatch shape CLAUDE.md's `.get()` ban closes for smart pointers (#14) -- a
// raw pointer has nowhere legal to go once raw pointers are otherwise forbidden.
bool isKnownStringMethod(const std::string& methodName);

// Is `type` a `std::map<K,V>` (or bare `map<K,V>`) shape? (Phase 2.6 map slice.)
bool isMapType(const std::string& type);

// "std::map<int, string>" -> "int". Returns "" if `type` isn't a map<...> shape.
std::string mapKeyType(const std::string& type);

// "std::map<int, string>" -> "string". Returns "" if `type` isn't a map<...> shape.
std::string mapValueType(const std::string& type);

// Is `type` one of CeeLing's primitive value types (int/float/double/bool/string)?
// Used to reject class-typed map keys/values this slice -- a map's element ownership
// question (unique_ptr vs shared_ptr per entry, the same problem vector slice 1
// solved for push_back) is deliberately deferred; primitives sidestep it entirely
// since they're always plain-copy values.
bool isPrimitiveType(const std::string& type);

} // namespace cling
