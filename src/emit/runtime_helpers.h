#pragma once

#include <set>
#include <string>

namespace cling {

// The C++ text of the runtime support that generated programs carry. Each function returns one self-contained,
// include-guarded block that AstEmitter writes into the output only when the program needs it.

std::string spanHelperDefinition();

// The definitions of the runtime helpers named in `used` (see isKnownRuntimeHelper).
std::string requiredRuntimeHelperDefinitions(const std::set<std::string>& used);

// cling::Channel<T>, emitted into a generated .cpp / .h that mentions a Channel.
std::string channelHelperDefinition();

std::string vectorAlgorithmHelperDefinition();

// `cling::copyWithin`, what `v.copyWithin(dst, src, len)` lowers to. Bytes in [dst, dst+len) end up as with
// std::memmove. The SSE2 fast path copies whole 16-byte chunks and writes the last chunk with one 16-byte store,
// so bytes just past dst+len may be overwritten; do not rely on them. It runs only when every chunk lies inside
// the vector, otherwise (and without SSE2) it uses std::memmove.
std::string copyWithinHelperDefinition();

// Bounds check for `v[i]` / `s[i]` (cling::at). An index outside [0, size), negative ones included, prints the
// .clg line and aborts. Types with a key_type (std::map) keep operator[]'s insert-or-update meaning and are
// not checked. Emitted unless --no-bounds-checks was given. It aborts rather than throws so that user code
// cannot swallow it with `catch (string e)`.
std::string boundsCheckHelperDefinition();

// Emitted by --pool-shared: a per-thread free-list allocator used through std::allocate_shared.
std::string poolAllocatorHelperDefinition();

// Null check for dereferencing an object handle (cling::nn): aborts with the .clg line instead of dereferencing
// null. Aborts rather than throws, for the same reason as the bounds check.
std::string nullCheckHelperDefinition();

} // namespace cling
