#pragma once

#include <set>
#include <string>
#include <vector>

namespace cling {

// Hand-written borrow/ownership summaries for a minimal std::vector<T> allow-list
// (Phase 2.6 slice 1). This is the first real entry in the design notes' library-summary
// allow-list mechanism -- the same one EscapeHatchChecker's default-reject-unknown-
// calls check (Phase 1) demands for anything CeeLing didn't compile itself. Grows
// incrementally; only std::vector<T>'s push_back/size/begin/end are covered here.
// operator[] and iterator-invalidation summaries are deferred to the next slice.

// Is `type` a `std::vector<...>` (or bare `vector<...>`) shape?
bool isVectorType(const std::string& type);

// "std::vector<Node>" -> "Node". Returns "" if `type` isn't a vector<...> shape.
std::string vectorElementType(const std::string& type);

// Is `methodName` a known std::vector<T> method this allow-list covers?
// `resize` is listed here but EscapeHatchChecker additionally restricts it to
// vectors whose element type is primitive: resize(n) on a vector of objects
// would fill it with null smart pointers (threat #9, whose runtime check does
// not exist yet), and resize(n, x) would duplicate an owner. Chosen over
// `assign(n, value)`, which also has an iterator overload -- and
// `v.assign(v.begin(), v.end())` (iterators into the same vector) is
// undefined behavior. resize has no iterator form at all.
//
// `reserve` is likewise restricted to primitive-element vectors (see
// EscapeHatchChecker::isKnownCallable), for the same reason resize is, even
// though reserve itself never constructs a NEW element (it only grows
// capacity, never size()) -- kept symmetric with resize/copyWithin rather
// than allowing it only for object elements, since v1 has no motivating case
// for reserve on an object-typed vector and the container-argument
// restriction is already the established pattern on this page.
//
// `copyWithin(dstStart, srcStart, len)` -- Rust `slice::copy_within`'s exact
// shape and semantics, not a novel one: there is no real std::vector method
// by this name, so AstEmitter's FunctionCall branch generates a
// std::memmove call for it instead of a plain pass-through (the one method
// on this page that needs its own codegen). Copies `len` elements from
// `srcStart` to `dstStart` within the SAME vector.
//
// memmove correctly handles ranges that overlap in the ORDINARY sense (the
// destination range partially covers the source range, e.g. copying
// [0,5) to [2,7)) -- it is specified to behave as if the source were read
// into a temporary buffer first, then written out, so partial overlap of
// that kind is always safe. It does NOT and CANNOT produce a
// self-repeating pattern: copyWithin(dstStart=2, srcStart=0, len=5) with
// only 2 real source elements does NOT yield [a,b,a,b,a] (repeating the
// 2-element pattern to fill 5 slots) -- it reads the 5 SOURCE elements
// (2 real ones plus 3 elements of whatever was already at [2,5), since
// that's what "read len elements starting at srcStart" means when
// dstStart < srcStart + len) and writes them to the destination, once,
// with no further repetition. Confirmed via direct probe before writing
// this comment (a real, not hypothetical, bug caught before it shipped):
// this is EXACTLY DEFLATE's back-reference shape when the match distance
// is SHORTER than the match length (a run-length-style repeat, e.g.
// distance=2 length=5 on "AB" must yield "ABABA") -- copyWithin is the
// wrong primitive for that case. A caller needing that self-repeating
// behavior must detect `dist < len` itself and fall back to a different
// strategy (e.g. an explicit per-element loop, or repeated calls to
// copyWithin with a growing prefix) -- copyWithin itself makes no attempt
// to detect or handle this, exactly like Rust's copy_within doesn't either
// (it has the identical memmove-based semantics and the identical
// limitation).
//
// Restricted to primitive elements, same reasoning as resize/reserve: it
// computes a byte count from the element type, which is meaningless for
// an object element, and there is no motivating case for it there yet
// regardless.
bool isKnownVectorMethod(const std::string& methodName);

// Does calling `methodName` on a vector MOVE its argument at position `argIndex`
// (consume it, the default for any CeeLing call), or does it SHARE (link, like
// storing into a field -- the argument remains a live, usable owner afterward)?
// Only `push_back` is modeled for this slice; everything else defaults to "moves"
// (the safe default) since no other vector method covered here takes an
// object-typed argument.
bool vectorMethodSharesArg(const std::string& methodName, size_t argIndex);

// Does calling `methodName` on a vector potentially reallocate its backing store,
// invalidating every existing reference/iterator into the container (the design notes
// threat #4)? `push_back` and `resize` may reallocate; `pop_back` and a
// shrinking `resize` destroy elements. `insert`/`erase` will extend this set
// once they exist. `size`/`begin`/`end` are read-only and never invalidate.
//
// `reserve` IS in this set (changed 2026-11-02; it used to be excluded). The C++ standard
// says a `reserve` that grows capacity invalidates every reference, pointer and iterator
// into the vector, and the signature scan in tools/std-scan-experiment finds `reserve`
// among the 13 non-const, no-const-twin members of `std::vector` (exactly the standard's
// list). The old argument -- "size() and every element's value are unchanged, so no
// observer can notice" -- was about an observer of the vector OBJECT; an observer of an
// ELEMENT does notice, because the element moved. It stayed harmless only because `reserve`
// is admitted on primitive-element vectors alone and a `ref` to a primitive element does
// not compile. One rule for all mutators is simpler to defend than that coincidence.
bool vectorMethodInvalidatesReferences(const std::string& methodName);

// Why a `std::vector` / `string` method is NOT admitted, as one sentence for the error message ("it returns an iterator, and
// iterators are not tracked yet ..."). Empty when the method is admitted. Admission is computed from the scan of the standard
// library (facts/std-vector.clfacts, std-string.clfacts) by a rule in library_summary.cpp; there is no list of names.
std::string vectorMethodRefusal(const std::string& methodName);

// `string_view` (facts/std-string_view.clfacts): a non-owning view of a string. Only a local or a parameter may have this type (the checker
// follows a view to the string it was made from; a field, a return value or a container element would escape that), and its methods are
// admitted by the same rule, except that a `view` result (substr) is honoured: it is a view of the same owner.
bool isStringViewType(const std::string& type);
bool isKnownStringViewMethod(const std::string& methodName);
std::string stringViewMethodRefusal(const std::string& methodName);
bool stringViewMethodReturnsView(const std::string& methodName);

// `span<T>` / `std::span<T>` (facts/cling-span.clfacts, hand-written): CeeLing's OWN non-owning view of a vector's elements, lowered to
// cling::span (spanHelperDefinition). std::span itself is C++20 and the generated code is C++17, so there is nothing to scan. T is a number, bool,
// char, byte or int64. Like a string_view it is an observer of the vector it was made from, and only a local or a parameter may have this type.
bool isSpanType(const std::string& type);
std::string spanElementType(const std::string& type);      // the element type without a `const`
bool isSpanConstType(const std::string& type);                // `std::span<const int>`: a read-only view
bool isSpanElementAllowed(const std::string& elem);
bool isKnownSpanMethod(const std::string& methodName);
std::string spanMethodRefusal(const std::string& methodName);
bool spanMethodReturnsView(const std::string& methodName);
std::string spanHelperDefinition();
// A view of either kind (string_view or span): only a local variable or a parameter may have such a type.
bool isViewType(const std::string& type);

// `std::optional<T>` (facts/std-optional.clfacts, scanned with T = int and T = std::string as witnesses): a value that may be absent. T is
// restricted to a primitive or a string, so an optional is a plain value (copied, never an observer target). `value()` on an empty optional
// throws std::bad_optional_access (uncaught: the program terminates, no undefined behaviour); `*o` and `o->` (undefined behaviour when empty) are refused.
bool isOptionalType(const std::string& type);
std::string optionalElementType(const std::string& type);
bool isOptionalElementAllowed(const std::string& elem);
bool isKnownOptionalMethod(const std::string& methodName);
std::string optionalMethodRefusal(const std::string& methodName);

// The same for `std::map<K, V>` (facts/std-map.clfacts): which methods may be called on a map, and why not. A map has no
// methods other than these and the subscript.
bool isKnownMapMethod(const std::string& methodName);
std::string mapMethodRefusal(const std::string& methodName);
bool mapMethodInvalidatesReferences(const std::string& methodName);
std::string stringMethodRefusal(const std::string& methodName);

// The same question for a `string`. The standard: `push_back`, `append`, `clear`, `reserve`
// (all four admitted by isKnownStringMethod) may invalidate references, pointers and iterators
// into the string. **It is live since 2026-11-03**: a `string_view` is an observer of its string
// (DanglingChecker::bindStringView), and DanglingChecker::invalidateContainerObservers consults this function, so a
// mutating call on the string kills the view. (A `ref string` observer is still never registered: `ref string t = s;
// s.append("c"); use(t)` is accepted; it is a pointer to the string OBJECT, which stays valid.)
bool stringMethodInvalidatesReferences(const std::string& methodName);

// Is `type` CeeLing's `string` primitive (bare keyword, or `std::string`/`string`
// written explicitly)? `string` is a value type -- unlike `std::vector<T>`, it never
// participates in DanglingChecker's move/observer tracking (isObjectType), the same
// way `int`/`bool` don't; this is purely a codegen + allow-list question of which
// name maps to `std::string` and which methods are safe to call on it.
bool isStringType(const std::string& type);

// Is `methodName` a known std::string method this allow-list covers? Deliberately
// excludes `c_str()` (and `data()`): both extract a raw `const char*`, the same
// escape-hatch shape the design notes' `.get()` ban closes for smart pointers (#14) -- a
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

// `int(x)`, `double(x)`, `float(x)`, `char(x)`, `byte(x)`, `int64(x)`: a conversion between two
// primitive types. The name must be one of these AND the single argument must not be an object,
// container or string (EscapeHatchChecker). `bool(x)` is left out: a non-zero test is `x != 0`.
bool isPrimitiveConversionName(const std::string& name);

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
//
// "cling::readFileToBytes" (whole-file slurp as std::vector<int>, one element per
// byte, empty vector on any failure) is the binary-safe sibling of
// readFileToString -- found necessary while building zlib-cling's inflate.clg:
// readFileToString's std::ifstream is opened WITHOUT std::ios::binary, so on
// Windows it performs CRLF translation, which silently corrupts arbitrary binary
// data containing a literal 0x0D byte (confirmed: a real zlib-compressed stream
// can and does contain 0x0D in its payload). A DIFFERENT helper rather than a
// flag on the existing one because the return TYPE differs (string vs
// vector<int>), which is a real overload-shape difference this allow-list has no
// mechanism to express on a single name. Each byte is widened via
// static_cast<unsigned char> before landing in the int, so callers never hit the
// signed-char sign-extension footgun (0xFF must read back as 255, not -1) --
// pushed into the shim itself rather than left for every .clg caller to remember
// `& 255`.
bool isKnownRuntimeHelper(const std::string& qualifiedName);

// EXPLORATORY Eigen spike (see CeeLing/the internal notes' Eigen entry) -- a narrow,
// hardcoded allow-list proving Eigen::MatrixXd can be CALLED from CeeLing at
// all, taken as the "whitelist first" step before the real, general
// third-party-borrow-summary mechanism (Part B, still just a plan) exists.
// Deliberately provides ZERO dangling/borrow safety: a call accepted here has
// NO FunctionSummary, so DanglingChecker's resolveCallReturnAlias never finds
// one and treats the result as an ordinary fresh value with no aliasing --
// correct for `Eigen::MatrixXd c = a * b;` (a real, safe, non-borrowing
// pattern, confirmed safe specifically because CeeLing's grammar has no
// `auto`, so every local is already the "concrete type forces evaluation"
// case Eigen's own docs call the safe idiom).
bool isKnownExternalClassType(const std::string& type);
bool isKnownExternalOperator(const std::string& type, const std::string& operatorStr);

// Eigen methods known to return a VIEW that BORROWS `self` (the receiver),
// e.g. `a.block(...)`/`a.row(i)`/`a.transpose()` -- NOT a fresh owned value.
// See docs/design/eigen-views.md for the full justification; summary: Eigen's own
// return-type naming convention makes this legible WITHOUT parsing the real
// (macro-laden, CRTP, `.inc`-file) signatures that produce it --
// `col()`/`row()`/`block()` etc. all return a type aliasing Eigen's `Block`/
// `Transpose`/`Ref`/`Map`/`Diagonal`/`Reverse` family, whose own constructors
// take a reference to their source and STORE it (confirmed by reading
// `eigen/Eigen/src/Core/Block.h`/`eigen/Eigen/src/plugins/BlockMethods.inc`
// directly). Unlike `isKnownExternalOperator` above, a call accepted via this
// list DOES get real borrow tracking -- DanglingChecker registers its result
// as an observer of the receiver (see
// DanglingChecker::resolveBorrowingMethodReceiver), so reassigning/moving the
// receiver afterward and then using the borrowed result is a real, caught
// dangling error, not silently accepted. Deliberately narrow (v1): only the
// single-receiver, no-multi-operand-borrow shape (`receiver.method(args) ->
// borrows self`); does not cover `operator+(a, b)`-style multi-operand
// borrows (moot for CeeLing -- see the `auto` finding in
// docs/design/eigen-views.md) or chained views (`a.block(...).transpose()`, which
// are conservatively rejected as unresolvable receivers, not specially
// handled).
bool isKnownBorrowingMethod(const std::string& className, const std::string& methodName);

// `opaque Name` in a fact pack: an external class nobody wrote method facts for ("trusted", the conservative model of
// docs/design/third-party-inference.md). Every method may be called. A method with no `fresh`/`borrow` fact is assumed to (a) return something
// that borrows the receiver, so `Grid w = g.anything();` makes `w` an observer of `g`, and (b) change the receiver, so it kills
// every existing observer of the receiver. `fresh` and `borrow` facts refine this per method. The class's internals are never examined:
// what the library does with what it is handed is trusted, exactly as for an `ext_fn`.
bool isOpaqueExternalClass(const std::string& type);

// Is this call to `className::methodName` one that falls back to the opaque default (an opaque class, no fact for the method)?
bool isConservativeOpaqueCall(const std::string& className, const std::string& methodName);

// A method of an external class whose result is fresh (a number, an owned object) and borrows nothing: `fresh Class::method` in a
// fact pack. Like a borrowing method it may be called; unlike one, nothing is tracked about its result.
bool isKnownFreshMethod(const std::string& className, const std::string& methodName);

// `mutator Class::method` in a fact pack: the method changes the receiver (so every observer of it dies) but returns nothing that
// borrows from it (void, a number, a fresh value) and retains none of its arguments. What cling-scan writes for a non-const method
// whose signature proves that much.
bool isKnownMutatorMethod(const std::string& className, const std::string& methodName);

// Does calling this method of an external class kill the observers of its receiver? True for a `mutator` fact and for an opaque
// class's unlisted method.
bool callKillsReceiverObservers(const std::string& className, const std::string& methodName);

// Every external class type the fact packs declare (`class Name`), for the analyzers that must treat them as value types.
std::vector<std::string> knownExternalClassTypes();

// The C++ source text of the runtime helper(s) actually called in this program
// (only the ones in `used`, so a program that never calls one gets no injected
// code). Each definition is self-contained and requires only <fstream>/<sstream>/
// <string>, already covered by AstEmitter's unconditional-include list.
std::string requiredRuntimeHelperDefinitions(const std::set<std::string>& used);

// Definition of `cling::copyWithin`, the runtime helper `v.copyWithin(dst,
// src, len)` lowers to (emitted once per translation unit that uses it,
// macro-guarded). Semantics for [dst, dst+len) are exactly std::memmove's
// (Rust slice::copy_within's): those bytes always end up correct. Why a
// helper instead of a bare std::memmove call: the motivating caller
// (zlib-cling's match copy) copies ~7 bytes on average, where a library
// memmove call is mostly call overhead plus length-dependent branches --
// measured at ~8% of total decompress time. The fast path copies whole
// 16-byte chunks and writes the final (<=16-byte) chunk as ONE unconditional
// 16-byte SIMD store: bytes strictly past dst+len that happen to share that
// last 16-byte store are NOT preserved -- they are overwritten with the
// corresponding source bytes, an unspecified value from the caller's
// perspective (never relied upon; see NOTE below). It runs only when every
// 16-byte chunk it touches lies inside the vector (bounds checked against
// v.size(), so it never reads/writes past the container) and the ranges
// don't overlap (or len <= 16, where all loads happen before the single
// store, which is memmove-correct for any overlap); anything else takes
// std::memmove, which never touches a byte outside [dst,dst+len).
//
// NOTE: an earlier version of this helper preserved bytes past dst+len (via
// an SSE2 blend of the old and new values). That guarantee was never part
// of CeeLing's own safety contract (which is specifically about dangling
// references/bounds/use-after-move, not this), was not tested by any
// tests/safety/*.clg fixture, and cost real decompress throughput in
// zlib-cling's own hot path (profiled independently on Windows and Linux at
// ~21-24% of InflateState::inflateFast()'s sampled cycles; removing it
// measured a real, reproducible ~1-3% end-to-end decompress speedup -- see
// git log for this line). It was removed. A caller that needs bytes past
// its own copyWithin call's destination range to be unmodified must ensure
// nothing it cares about lives there (e.g. size the vector so dst+len is the
// logical end, or don't rely on padding contents) -- exactly how zlib-cling's
// own InflateState already uses this (its `output` vector is deliberately
// over-resized with real padding past every mid-loop copy's destination,
// for exactly this reason). On a platform without CLING_COPY_WITHIN_SSE2
// (no SSE2/_M_X64/_M_IX86_FP >= 2), the whole fast path compiles out and
// std::memmove is used unconditionally -- which DOES fully preserve bytes
// past dst+len, since memmove never touches anything outside its own
// [dst,dst+len)/[src,src+len) ranges. This cross-platform difference is
// deliberate, not a bug: don't assert specific padding values in a fixture,
// since the answer differs by platform.
std::string copyWithinHelperDefinition();

// CeeLing's own whole-vector algorithms, `v.sort()`, `v.reverse()`, `v.fill(x)`, `v.indexOf(x)` (-1 when absent), `v.contains(x)`, on a vector of
// primitives. No iterators are involved, so nothing can dangle; none changes size(). `sort` orders NaN last (std::sort with NaN is undefined
// behaviour). isVectorAlgorithm(name) names them, vectorAlgorithmArity(name) is their argument count, vectorAlgorithmHelperDefinition() the runtime.
bool isVectorAlgorithm(const std::string& methodName);
size_t vectorAlgorithmArity(const std::string& methodName);
std::string vectorAlgorithmHelperDefinition();

// ---- Channel<T> -----------------------------------------------------------------------------------------------
// `Channel<T>`: a built-in handle to a queue through which threads hand each other PLAIN VALUES (the same element types a `spawn` may
// capture: primitives, string, vectors of primitives, structs of those). Copying the handle -- by assignment, as an argument, into a
// `spawn` capture -- gives another handle to the SAME queue; the queue itself is guarded by a mutex, and what goes through it is
// copies. It is the one sanctioned piece of state that a spawned job shares with its creator (the design notes, spawn).

// Is `type` a `Channel<...>` shape?
bool isChannelType(const std::string& type);

// "Channel<int>" -> "int". Returns "" if `type` isn't a Channel<...> shape.
std::string channelElementType(const std::string& type);

// The methods a Channel has, with their argument counts: send(v), wait(ms), take(), size(), close(), isClosed().
bool isKnownChannelMethod(const std::string& methodName, size_t argCount);

// C++ source for cling::Channel<T> (include-guarded; emitted into a generated .cpp / .h that mentions a Channel).
std::string channelHelperDefinition();

// Runtime null check for dereferencing a smart pointer that may legitimately be empty -- today
// the result of locking a weak_ptr field (cling::nn). Aborts with a message naming the source
// line instead of dereferencing null. Emitted only into files that declare a weak field.
// abort(), not throw: a thrown error could be swallowed by a `catch (string e)` in user code,
// and the whole point of this check is that it cannot be silently ignored.
// Emitted by --pool-shared: a per-thread free-list allocator used through std::allocate_shared.
std::string poolAllocatorHelperDefinition();
std::string nullCheckHelperDefinition();

// Runtime bounds check for `v[i]` / `s[i]` on std::vector and std::string (cling::at). An index
// outside [0, size) -- including every negative index -- prints the .clg line and aborts. Types
// that have a key_type (std::map) keep operator[]'s insert-or-update meaning and are NOT checked,
// so the emitter can wrap any non-class subscript without having to know the receiver's type.
// Emitted unless --no-bounds-checks was given. abort(), not throw, for the same reason as nn().
std::string boundsCheckHelperDefinition();

} // namespace cling
