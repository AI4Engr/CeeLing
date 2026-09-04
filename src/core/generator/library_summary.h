#pragma once

#include <set>
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

// Is `type` CeeLing's `ostream` type (bare keyword only -- no `std::ostream`
// spelling accepted, unlike isStringType's flexibility, since no fixture needs
// the qualified form written explicitly)? Motivated by a free `operator<<`
// definition needing a real, by-REFERENCE `std::ostream&` parameter/return type
// (yaml-cpp's `std::ostream& operator<<(std::ostream&, const Node&)`) -- a plain
// value type like `string`/`int` never fits here, since std::ostream's copy
// constructor is deleted (passing it by value is not merely suboptimal, it does
// not compile at all). AstEmitter checks this to force `std::ostream&` at every
// parameter/return-type site, never falling through to the ordinary
// value/class-type wrapping logic.
bool isOstreamType(const std::string& type);

// Is `type` a `std::map<K,V>` (or bare `map<K,V>`) shape? (Phase 2.6 map slice.)
bool isMapType(const std::string& type);

// "std::map<int, string>" -> "int". Returns "" if `type` isn't a map<...> shape.
std::string mapKeyType(const std::string& type);

// "std::map<int, string>" -> "string". Returns "" if `type` isn't a map<...> shape.
std::string mapValueType(const std::string& type);

// Is `type` one of CeeLing's primitive value types (int/float/double/bool/string/
// char)? Used to reject class-typed map keys/values this slice -- a map's element
// ownership question (unique_ptr vs shared_ptr per entry, the same problem vector
// slice 1 solved for push_back) is deliberately deferred; primitives sidestep it
// entirely since they're always plain-copy values.
bool isPrimitiveType(const std::string& type);

// Is `type` CeeLing's `char` primitive? A pure value type like int/bool -- never
// move/observer tracked (DanglingChecker::isObjectType), never a pointer/array (no
// char*/char[] production exists in the grammar at all). Its only source this
// slice is `string[i]` (see AstEmitter/DanglingChecker/EscapeHatchChecker's
// SubscriptExpression string-receiver branch) and char literals (`'x'`).
bool isCharType(const std::string& type);

// Is `qualifiedName` (e.g. "std::stoi") a known qualified FREE function this
// allow-list covers? Grown incrementally, same discipline as every other allow-
// list here -- currently just the two yaml-cling needs for `.as<int>()`/
// `.as<double>()`. Both take a `string` and return a primitive: no object-typed
// argument or return value, so unlike the vector/string method allow-lists above,
// no share-vs-move or invalidation summary is needed for these.
bool isKnownQualifiedFreeFunction(const std::string& qualifiedName);

// Is `qualifiedName` a CeeLing-provided runtime helper (e.g.
// "cling::readFileToString") rather than a real std:: function? These are tiny,
// hand-written C++ shims AstEmitter auto-injects into every generated file that
// calls one (see requiredRuntimeHelperDefinitions), used only when CeeLing's
// language model has no way to express the operation itself (no object
// construction/method-call support for foreign types like std::ifstream) but the
// operation is a single, well-defined, ownership-free primitive (string/bool in,
// string/bool out) that a real std:: free function doesn't already cover in one
// call. Takes/returns only primitives, same as isKnownQualifiedFreeFunction's set
// -- no share-vs-move or invalidation summary needed here either.
//
// Currently: "cling::readFileToString" (whole-file slurp, "" on any failure) and
// "cling::fileExists" (can the path be opened for reading at all -- lets a caller
// distinguish "file not found" from "file opened fine but is empty", which
// readFileToString's own return value alone cannot, since both cases return "").
bool isKnownRuntimeHelper(const std::string& qualifiedName);

// The C++ source text of the runtime helper(s) actually called in this program
// (only the ones in `used`, so a program that never calls one gets no injected
// code). Each definition is self-contained and requires only <fstream>/<sstream>/
// <string>, already covered by AstEmitter's unconditional-include list.
std::string requiredRuntimeHelperDefinitions(const std::set<std::string>& used);

} // namespace cling
