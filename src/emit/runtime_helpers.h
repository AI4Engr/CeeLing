#pragma once

#include <set>
#include <string>

namespace cling {

// The C++ text of the runtime support that generated programs carry. Each function returns one self-contained, include-guarded block
// that AstEmitter writes into the generated file (or header) only when the program needs it. What decides WHICH helper a program needs is
// in library_summary.h (the names) and AstEmitter (the scan).

std::string spanHelperDefinition();

// The C++ source text of the runtime helper(s) actually called in this program
// (only the ones in `used`, so a program that never calls one gets no injected
// code). Each definition is self-contained and requires only <fstream>/<sstream>/
// <string>, already covered by AstEmitter's unconditional-include list.
std::string requiredRuntimeHelperDefinitions(const std::set<std::string>& used);

// C++ source for cling::Channel<T> (include-guarded; emitted into a generated .cpp / .h that mentions a Channel).
std::string channelHelperDefinition();

std::string vectorAlgorithmHelperDefinition();

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

// Runtime bounds check for `v[i]` / `s[i]` on std::vector and std::string (cling::at). An index
// outside [0, size) -- including every negative index -- prints the .clg line and aborts. Types
// that have a key_type (std::map) keep operator[]'s insert-or-update meaning and are NOT checked,
// so the emitter can wrap any non-class subscript without having to know the receiver's type.
// Emitted unless --no-bounds-checks was given. abort(), not throw, for the same reason as nn().
std::string boundsCheckHelperDefinition();

// Runtime null check for dereferencing a smart pointer that may legitimately be empty -- today
// the result of locking a weak_ptr field (cling::nn). Aborts with a message naming the source
// line instead of dereferencing null. Emitted only into files that declare a weak field.
// abort(), not throw: a thrown error could be swallowed by a `catch (string e)` in user code,
// and the whole point of this check is that it cannot be silently ignored.
// Emitted by --pool-shared: a per-thread free-list allocator used through std::allocate_shared.
std::string poolAllocatorHelperDefinition();

std::string nullCheckHelperDefinition();

} // namespace cling
