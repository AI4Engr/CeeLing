# 4. Memory-Safety Threat Model

"No raw pointers + all smart pointers" is **necessary but far from sufficient.**
Smart pointers solve *who calls `delete`* (leaks, double-free) — they do **not** stop
dangling, null, use-after-move, out-of-bounds, or type confusion. Reaching ~99.99%
memory safety means closing every item below.

## Strategy decisions (settled)

- **Static + runtime backstop.** Dangling / type / escape-hatches are caught
  statically. Checks that are impractical to prove statically (bounds, null deref)
  are enforced by **compiler-inserted runtime checks** that panic/throw — the same
  pragmatic path as cppfront and Rust. **Pure-static is NOT required**; it would
  over-reject to the point of unusability.
- **Single-thread scope for v1.** Data-race / concurrency safety (Rust's Send/Sync
  half) is **explicitly out of scope for v1.** The contract guarantees single-threaded
  memory safety only; this limitation is stated honestly to users.

## The threat table

| # | Threat | How to block | Enforce |
|---|--------|--------------|---------|
| **Dangling references (the core)** | | | |
| 1 | Observer / non-owning reference outlives object | provenance + lifetime | static |
| 2 | Return reference/pointer to local | return-value provenance | static |
| 3 | Reference stored in a longer-lived field (escape) | structural + provenance | static |
| 4 | **Iterator invalidation** (`push_back` reallocs, then `*it`) | summary marks mutators as invalidating a container's iterators | static |
| 5 | **`string_view` / `span` dangles** (view of a temporary) | temporary-lifetime + provenance on view types | static |
| 6 | Expression-template dangle (Eigen `auto e = A+B`) | `auto` vs concrete-type provenance | static |
| 7 | Lambda captures `[&]` local, then escapes | **DONE** — closures exist (`closure IFace [caps] (params) {...}`), but capture is **by value only**; `[&]` is not detected-and-rejected, it is **unrepresentable in the grammar** (the capture list is bare identifiers, no capture-mode syntax at all). See Chapter 18a. | static (by construction) |
| 8 | Dangling temporary bound to `const&` subobject | temporary-lifetime rule | static |
| **Other UB (not dangling)** | | | |
| 9 | Null deref (`shared_ptr`/`weak_ptr.lock()` may be empty) | non-null typing where provable, else runtime check | static + runtime |
| 10 | **Use-after-move** (auto-`std::move` *creates* this risk) | moved-from state tracking | static |
| 11 | Array / container out-of-bounds `v[i]` | compiler-inserted bounds check | runtime |
| 12 | `shared_ptr` reference cycle → leak | cycle detection → `weak_ptr` | static |
| **Escape hatches that MUST be sealed** (one hole ⇒ contract void) | | | |
| 13 | `reinterpret_cast` / C-style cast | forbid | static |
| 14 | `.get()` extracts a raw pointer (back door) | **forbid** (raw pointers have nowhere legal to go) | static |
| 15 | Raw `new`/`delete`, raw `T*` | **forbid** (`delete` banned outright) | static |
| 16 | `const_cast` (then write) | forbid | static |
| 17 | `union` / `variant` type confusion | forbid raw union; guard variant | static |
| 18 | Read of uninitialized variable | require definite-init | static |
| **Out of scope (v1)** | | | |
| 19 | Data races / concurrency | — (single-thread contract) | out of scope |

## Key implications

- **Auto-`std::move` and use-after-move are coupled** (#10). Auto-inserting
  `std::move` *manufactures* moved-from values, so use-after-move tracking is a
  prerequisite, not an optional extra.
- **Iterator invalidation (#4)** is the highest-frequency C++-specific dangling source
  that Java does not have. The library summary table must encode which methods
  invalidate which iterators.
- **`.get()` (#14) is the smart-pointer back door.** Leaving it open silently voids
  the entire contract — one `.get()` reintroduces a raw pointer.
- **99.99% is reached by static-catch + runtime-backstop**, not pure static proof.
- **Closures (#7) close by elimination, not by analysis** — the same strategy as
  `ref` fields (#3): rather than build a capture-provenance-vs-lambda-lifetime
  analysis (the originally planned approach for #7, and the harder of the two
  options), by-reference capture was made syntactically impossible. This is a
  real trade-off, not a free win: it means a closure can never observe an
  enclosing local without taking ownership of a copy (or, for a `shared_ptr`-
  backed object, a share) of it — see Chapter 18a for what this does and does
  not let a caller express, and why by-value-only still made closures a
  genuine language-expressiveness milestone (Begin/End-shaped APIs collapse
  to one call; callbacks/strategy objects become directly expressible)
  despite that restriction.

## "Enough for modern C++"?

Solving *dangling + bounds alone* is **not** enough. The minimum set for
single-threaded 99.99% memory safety is:

> **dangling (#1–#8) + null (#9) + use-after-move (#10) + bounds (#11) + sealed
> escape hatches (#13–#18).**

Null and use-after-move are just as high-frequency as bounds and cannot be skipped.
Beyond memory safety, day-to-day usability also needs **language expressiveness**
(generics, lambdas, range-for, operator overloading, inheritance) — the foundation is
safety; the house on top is expressiveness.
