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

} // namespace cling
