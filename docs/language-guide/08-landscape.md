# 8. Landscape: Rust, Circle, and C++ Profiles

Why does CeeLing exist when Rust, Sean Baxter's Circle (Safe C++), and the ISO C++
"Profiles" effort all target memory safety? Because **none of them occupies the point
CeeLing aims for**: *Java-like ergonomics + reuse of the C++ ecosystem (Eigen/std) +
dangling safety.* Each competitor is missing at least one of those three.

## The scorecard

| | Familiar C++ syntax / Java-like | Uses C++ ecosystem (Eigen) safely | Dangling *points-to* implemented | Maturity |
|---|:---:|:---:|:---:|---|
| **Rust** | ✗ (`&`/`&mut`/lifetimes) | ✗ (no Eigen; own crates) | ✓ | shipped, mature |
| **Circle / Safe C++** | ✗ (full Rust model) | ✗ (only in `unsafe`) | ✓ | ISO effort stalled |
| **cppfront / Cpp2** | ✗ (new Syntax-2) | unknown | ✗ (not yet built) | experimental |
| **Carbon (Google)** | ✗ (new syntax) | ✗ (Rust-style path) | ✗ (design stage) | 1.0 not before ~2028 |
| **C++ Profiles** | ✓ | ✓ | ✗ (likely can't be built) | not in C++26 |
| **CeeLing** | ✓ (`ref`, auto pointers) | ✓ (plain C++ output) | Phase 2 (planned) | your call |

CeeLing is the only row aiming for all of *familiar syntax + safe Eigen + dangling
safety*. The others each miss a column — and those gaps are **structural**, not a
matter of maturity or open-sourcing.

> **The reassuring column is the third one.** Compile-time points-to / provenance
> tracking — the heart of dangling detection — is **not yet shipped by cppfront or
> Carbon either.** This is not a solved problem others finished long ago that CeeLing
> is chasing; it is a hard problem **nobody in this space has finished.** CeeLing is
> not behind here.

## Why the C++ committee can't fill this gap (2025 status)

In 2025 WG21 **rejected** Baxter's Safe C++ (Rust-style borrow checking) in favor of
**Profiles**. Baxter stepped back from the effort; Herb Sutter, the main Profiles
champion, stepped down from his committee role. The core deadlock is philosophical:
sound dangling checking needs **function coloring (`safe`/`unsafe`) + mutable-xor-shared
aliasing rules** — which the committee will not adopt because it would "turn C++ into
Rust." Without them, Profiles is widely doubted to be able to deliver real dangling
safety.

**CeeLing sidesteps the deadlock** because it defines a *restricted subset* rather than
trying to secure all existing C++. The committee cannot make that trade (they must keep
backward compatibility with every unsafe idiom); an independent language can. This is a
structural advantage of being a subset, and the window it opens is likely to stay open
for years.

## Why Circle can't use Eigen safely — the key difference

Circle *can* compile Eigen — it is a C++ superset. But in its **`safe` context, calling
legacy C++ (like Eigen) is banned by default.** To touch Eigen you must drop into an
explicit `unsafe` block, and Circle adopts Rust's blame model: *whoever wrote `unsafe`
owns the risk.* Inside `unsafe`, the borrow checker does **not** protect you — so
Eigen's dangling traps (expression-template `auto e = A+B`, `.block()` views) are
exactly as dangerous as in raw C++.

```cpp
// Circle Safe C++
safe void f() {
    unsafe {
        auto e = A + B;   // compiles — but borrow checker is OFF here; dangles unchecked
    }
}
```

The reason is fundamental: Circle's model enforces **exclusivity** (mutable-xor-shared),
and legacy C++ references (including Eigen's view/expression references) do **not** honor
exclusivity. So Eigen cannot be admitted into a `safe` context — it is pushed into
`unsafe` by the model itself. **Open-sourcing Circle would not change this**; it is a
model-level incompatibility, not a licensing one.

CeeLing does the opposite trade:

> **CeeLing checks only *liveness*, not *aliasing*.** It does not require exclusivity,
> so Eigen's reference-returning views are not a problem — a single summary
> (`block(&self) -> borrows self`) lets provenance track the dangle. **Giving up
> aliasing checks is exactly what lets CeeLing admit Eigen inside its safety boundary.**

So Eigen dangling lives **outside** Circle's safety boundary but **inside** CeeLing's —
and that boundary sits right on CeeLing's core use case (numeric code).

## cppfront / Cpp2 — closest in philosophy, two key differences

Herb Sutter's Cpp2 (compiled by cppfront to standard C++) is the nearest neighbor: it
also transpiles to C++, also does bounds / null / initialization checking, and also
**explicitly does not aim for 100% safety** ("Cpp2 will not enforce a memory-safety
subset 100%"). Two differences keep CeeLing distinct:

1. **cppfront introduces a large new syntax (Syntax-2)** — a second explicit goal is to
   *change* C++ syntax (`x: int = 0`, `(inout p: T)`). CeeLing does the opposite: keep
   familiar C++ syntax, stay Java-like. (This is the Ch. 1 differentiator, still true.)
2. **cppfront's dangling check is also not done yet — and stuck at the same spot.**
   Per its own overview, *"still not implemented yet is compile-time tracking of
   points-to information for each pointer."* That points-to tracking is precisely
   CeeLing's Phase 2. So on the one hard thing that matters most — compile-time
   dangling — even Herb Sutter's cppfront has not shipped it. Everyone is near the same
   starting line.

## Carbon (Google) — bigger, but no near-term threat

Carbon is far larger and more ambitious than CeeLing, targeting full Rust-level safety
(temporal *and* mutation safety, via the type system, no GC). Two facts defuse it as a
near-term concern:

1. **Carbon's memory safety is a *design effort, not a shipped feature* (2025).** Adding
   the safety design *delayed* the roadmap: 0.1 is not expected before late 2026, and
   **1.0 not before ~2028**. Today it is "experimental nightly, not ready for use."
2. **Carbon follows the Rust path (mutation/exclusivity safety)** — which means it is
   likely to hit **the same wall as Circle** with Eigen's aliasing view types. Even if
   Carbon ships, "safe Eigen" is probably outside its boundary too. And it is a *new
   syntax*, not Java-like.

The honest caveat: Carbon has Google's resources. If, years out, it delivers Java-like
ergonomics + safety + safe C++-ecosystem use, it could threaten CeeLing. But (a) its
Rust-style path likely conflicts with Eigen, (b) it is new syntax, and (c) 2028 is far
away. Near-term: no threat.

## Does open-sourcing a competitor kill CeeLing?

No — and the Eigen point is the proof. Rust is already open, free, mature, and
huge-ecosystem, yet it does not fill CeeLing's niche (no Eigen, not Java-like). A
weaker, open-sourced Circle would not either, because it *structurally* cannot use
Eigen safely. Open source changes acquisition cost, not what a tool *is*.

The only thing that removes CeeLing's reason to exist is **something that occupies all
three axes at once** — and today nothing does.

## The one real risk: becoming a worse Circle

The failure mode is self-inflicted. If CeeLing, chasing "more safety," adds aliasing
checks / exclusivity / function coloring / lifetime annotations, it walks straight into
Circle's wall — Eigen stops being usable in safe code, and the Java-like ergonomics
evaporate. **Holding the line at "liveness only, no aliasing" is what keeps the niche.**
That line is more important to defend than any single safety metric.
