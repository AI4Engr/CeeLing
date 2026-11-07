#pragma once

#include <set>
#include <string>
#include <vector>

namespace cling {

// What the checkers know about library types and calls: the allow-list for std::vector, string, map,
// optional, span and string_view, the external classes of fact packs, and CeeLing's own helpers.
// A call not covered here is rejected. Most entries are computed from facts/*.clfacts.

// Is `type` a `std::vector<...>` (or bare `vector<...>`) shape?
bool isVectorType(const std::string& type);

// "std::vector<Node>" -> "Node". Returns "" if `type` isn't a vector<...> shape.
std::string vectorElementType(const std::string& type);

// Is `methodName` a std::vector method the allow-list admits? resize, reserve and copyWithin are further
// restricted to primitive elements by EscapeHatchChecker: they would fill a vector of objects with null
// handles, duplicate an owner, or compute a byte count from an object type.
//
// copyWithin(dstStart, srcStart, len) has the semantics of Rust's slice::copy_within and lowers to
// std::memmove. Partial overlap is handled correctly, but it does not repeat a pattern:
// copyWithin(2, 0, 5) over [a, b] does not give [a, b, a, b, a]. A caller that needs that (a DEFLATE match
// with distance < length) must use a per-element loop.
bool isKnownVectorMethod(const std::string& methodName);

// Does calling `methodName` on a vector move its argument at `argIndex` (the default) or share it (the
// argument stays a live owner)? Only push_back shares.
bool vectorMethodSharesArg(const std::string& methodName, size_t argIndex);

// Does calling `methodName` on a vector invalidate every existing reference and iterator into it? True for
// the non-const members with no const twin (push_back, resize, reserve, clear, ...), which matches the
// standard's list. Used to kill observers (iterator invalidation).
bool vectorMethodInvalidatesReferences(const std::string& methodName);

// Why a std::vector / string method is not admitted, as one sentence for the error message. Empty when
// it is admitted. Admission is computed from facts/std-vector.clfacts and std-string.clfacts in
// library_summary.cpp; there is no list of names.
std::string vectorMethodRefusal(const std::string& methodName);

// `string_view` (facts/std-string_view.clfacts): a non-owning view of a string. Only a local or a parameter
// may have this type; the checker follows a view to its string. Methods are admitted by the same rule as
// string, except that a view-returning one (substr) is honoured as a view of the same owner.
bool isStringViewType(const std::string& type);
bool isKnownStringViewMethod(const std::string& methodName);
std::string stringViewMethodRefusal(const std::string& methodName);
bool stringViewMethodReturnsView(const std::string& methodName);

// `span<T>` / `std::span<T>` (facts/cling-span.clfacts, hand-written): CeeLing's own non-owning view of a
// vector, lowered to cling::span (std::span is C++20, the output is C++17). T is a number, bool, char, byte
// or int64. Like a string_view it observes the vector it was made from, and only a local or a parameter may have this type.
bool isSpanType(const std::string& type);
std::string spanElementType(const std::string& type);      // the element type without a `const`
bool isSpanConstType(const std::string& type);                // `std::span<const int>`: a read-only view
bool isSpanElementAllowed(const std::string& elem);
bool isKnownSpanMethod(const std::string& methodName);
std::string spanMethodRefusal(const std::string& methodName);
bool spanMethodReturnsView(const std::string& methodName);
// A view of either kind (string_view or span): only a local variable or a parameter may have such a type.
bool isViewType(const std::string& type);

// `std::optional<T>` (facts/std-optional.clfacts): a value that may be absent. T is a primitive or a string,
// so an optional is a plain value. `*o` and `o->` are refused.
bool isOptionalType(const std::string& type);
std::string optionalElementType(const std::string& type);
bool isOptionalElementAllowed(const std::string& elem);
bool isKnownOptionalMethod(const std::string& methodName);
std::string optionalMethodRefusal(const std::string& methodName);

// The same for `std::map<K, V>` (facts/std-map.clfacts): the methods a map has, besides the subscript, and why the others are refused.
bool isKnownMapMethod(const std::string& methodName);
std::string mapMethodRefusal(const std::string& methodName);
bool mapMethodInvalidatesReferences(const std::string& methodName);
std::string stringMethodRefusal(const std::string& methodName);

// Does calling this string method invalidate references into the string (push_back, append, clear,
// reserve)? A string_view observer of the string dies on it.
bool stringMethodInvalidatesReferences(const std::string& methodName);

// Is `type` CeeLing's `string` (bare keyword, `string` or `std::string`)? A value type: never move- or
// observer-tracked, like int.
bool isStringType(const std::string& type);

// Is `methodName` a std::string method the allow-list admits? c_str() and data() are excluded: they
// return a raw pointer, which has nowhere legal to go.
bool isKnownStringMethod(const std::string& methodName);

// Is `type` CeeLing's `ostream` (bare keyword only)? It must be passed by reference (`std::ostream&`),
// because std::ostream cannot be copied; the emitter forces that at every parameter and return type.
bool isOstreamType(const std::string& type);

// Is `type` a `std::map<K,V>` (or bare `map<K,V>`) shape?
bool isMapType(const std::string& type);

// "std::map<int, string>" -> "int". Returns "" if `type` isn't a map<...> shape.
std::string mapKeyType(const std::string& type);

// "std::map<int, string>" -> "string". Returns "" if `type` isn't a map<...> shape.
std::string mapValueType(const std::string& type);

// Is `type` a primitive value type (int/float/double/bool/string/char, ...)? Map keys and values must be
// primitive: they are plain copies, so no per-entry ownership question arises.
bool isPrimitiveType(const std::string& type);

// Is `type` CeeLing's `char`? A value type like int; there are no char pointers or arrays.
bool isCharType(const std::string& type);

// `int(x)`, `double(x)`, `float(x)`, `char(x)`, `byte(x)`, `int64(x)`: a conversion between two
// primitive types. The name must be one of these AND the single argument must not be an object,
// container or string (EscapeHatchChecker). `bool(x)` is left out: a non-zero test is `x != 0`.
bool isPrimitiveConversionName(const std::string& name);

// Is `qualifiedName` (e.g. "std::stoi") a qualified free function the allow-list covers? These take and
// return only primitives, so no move or invalidation summary is needed.
bool isKnownQualifiedFreeFunction(const std::string& qualifiedName);

// Is `qualifiedName` a runtime helper written by CeeLing (e.g. "cling::readFileToString") rather than a
// std:: function? The emitter injects the definition (src/emit/runtime_helpers) into any file that calls
// one. Helpers take and return only primitives or a vector of them. readFileToBytes is separate from
// readFileToString because it opens the file in binary mode and returns a vector of ints (0..255).
bool isKnownRuntimeHelper(const std::string& qualifiedName);

// A small hard-coded allow-list for calling Eigen::MatrixXd. A call accepted here has no borrow summary,
// so its result is treated as a fresh value (correct for `Eigen::MatrixXd c = a * b;`, where the concrete
// type forces evaluation).
bool isKnownExternalClassType(const std::string& type);
bool isKnownExternalOperator(const std::string& type, const std::string& operatorStr);

// Eigen methods that return a view borrowing the receiver (`a.block(...)`, `a.row(i)`, `a.transpose()`);
// see docs/design/eigen-views.md. The result is registered as an observer of the receiver, so using it after
// the receiver is reassigned or moved is rejected. Only the single-receiver shape is covered; chained views
// are rejected as unresolvable.
bool isKnownBorrowingMethod(const std::string& className, const std::string& methodName);

// `opaque Name` in a fact pack: an external class nobody wrote method facts for (the conservative model of
// docs/design/third-party-inference.md). Every method may be called. A method without a `fresh`/`borrow`
// fact is assumed to return something that borrows the receiver and to change the receiver (killing its
// observers). The library's internals are trusted, as for an `ext_fn`.
bool isOpaqueExternalClass(const std::string& type);

// Is this call to `className::methodName` one that falls back to the opaque default (an opaque class, no fact for the method)?
bool isConservativeOpaqueCall(const std::string& className, const std::string& methodName);

// `fresh Class::method` in a fact pack: the result is fresh (a number, an owned object) and borrows nothing.
bool isKnownFreshMethod(const std::string& className, const std::string& methodName);

// `mutator Class::method` in a fact pack: the method changes the receiver (its observers die) but returns
// nothing that borrows from it and retains none of its arguments.
bool isKnownMutatorMethod(const std::string& className, const std::string& methodName);

// Does calling this method of an external class kill the observers of its receiver? True for a `mutator` fact and for an opaque
// class's unlisted method.
bool callKillsReceiverObservers(const std::string& className, const std::string& methodName);

// Every external class type the fact packs declare (`class Name`), for the analyzers that must treat them as value types.
std::vector<std::string> knownExternalClassTypes();

// CeeLing's own whole-vector algorithms on a vector of primitives: `v.sort()` (NaN last), `v.reverse()`,
// `v.fill(x)`, `v.indexOf(x)` (-1 when absent), `v.contains(x)`. None uses iterators or changes size().
// vectorAlgorithmArity gives the argument count; the runtime is in src/emit/runtime_helpers.
bool isVectorAlgorithm(const std::string& methodName);
size_t vectorAlgorithmArity(const std::string& methodName);

// `Channel<T>`: a built-in handle to a mutex-guarded queue through which threads exchange plain values (the
// types a `spawn` may capture). Copying the handle gives another handle to the same queue. It is the one
// state a spawned job shares with its creator.

// Is `type` a `Channel<...>` shape?
bool isChannelType(const std::string& type);

// "Channel<int>" -> "int". Returns "" if `type` isn't a Channel<...> shape.
std::string channelElementType(const std::string& type);

// The methods a Channel has, with their argument counts: send(v), wait(ms), take(), size(), close(), isClosed().
bool isKnownChannelMethod(const std::string& methodName, size_t argCount);

} // namespace cling
