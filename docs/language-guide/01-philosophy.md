# 1. Philosophy — A Safe Subset of C++

CeeLing is **not** "a new language that compiles to C++." It is best understood as
**a restricted, memory-safe subset of C++** — in the same spirit as Rust being a
restricted subset of systems programming, or Herb Sutter's **cppfront / Syntax-2**
being a safer dialect of C++.

## The key differentiator from cppfront

> **cppfront's value is a cleaner *new syntax*. CeeLing's value is *compile-time
> dangling-reference safety* while keeping familiar C++ syntax.**

CeeLing deliberately avoids introducing a large new surface syntax. It keeps
ordinary C++-looking code and concentrates its guarantee on one thing: **no dangling
references can survive compilation.**

## The core bet

CeeLing's bet is a specific combination that no other language offers:

> **The C++ ecosystem (Eigen, and other mature C++ libraries) *plus* single-threaded
> memory safety.**

Rust cannot offer this — there is no Rust equivalent for Eigen's expression-template /
view semantics. So CeeLing is not "trying to catch up to Rust"; it trades an
acceptable, bounded amount of safety for **access to the C++ library ecosystem**.

## Core rules (the language boundary)

1. **Raw pointers are forbidden.** They are the primary escape hatch for unsafety.
   Raw `new`/`delete`, `T*`, and `.get()` are all rejected.
2. **Ownership is inferred, not annotated (by default).** Users write plain,
   Java-like code; the analyzer derives who owns what and which references alias whom.
   Explicit annotations (`borrow<T>`, etc.) may be *optionally* supported later as an
   expert escape hatch — never a requirement for the default experience.
3. **Default-reject, never default-allow.** Anything the analyzer cannot *prove* safe
   is rejected at compile time — including calls into library methods that are not in
   the summary allow-list. Being a subset means rejected code is *outside the
   language*, not a bug. This closes the safety boundary: there is no "unknown call"
   through which a dangling reference can leak past the checker.
4. **Ambiguous inference → conservative reject + error.** If ownership cannot be
   resolved to a single conclusion, CeeLing errors and asks the user to rewrite,
   rather than guessing.

## Roadmap framing

The supported subset can start small and grow. The library summary allow-list defines
how much real `std`/Eigen code compiles. Every increment is a complete,
self-consistent, memory-safe language — there is **no "useless until finished"
cliff**. A small-but-truly-safe subset is preferred over a large-but-leaky one.
