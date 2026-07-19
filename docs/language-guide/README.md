# CeeLing Language Guide

CeeLing is a **memory-safe, restricted subset of C++**. You write plain, Java-like
code; the compiler infers ownership, picks smart-pointer types, inserts moves, and
statically rejects code that could dangle — while still emitting ordinary C++ that
reuses the C++ ecosystem (`std`, Eigen, ...).

> **The safety contract:** *If a program compiles under CeeLing, it contains no
> dangling references* (single-threaded). See [Chapter 2](02-safety-contract.md).

## How to read this guide

Every **problem chapter** (10–17) follows the same three-part structure:

1. **The problem** — what goes wrong in raw C++ (with a broken example).
2. **How CeeLing solves it** — the rule/mechanism.
3. **Proof by example** — before (CeeLing) / after (generated C++), and what gets
   rejected vs. accepted.

## Status

These safety features are the **design target**, not the current state. As of now the
compiler only performs smart-pointer *type selection* and does **no** lifetime
checking. See [Chapter 9](09-status-and-roadmap.md).

## Part I — Foundations

1. [Philosophy — A Safe Subset of C++](01-philosophy.md)
2. [The Safety Contract](02-safety-contract.md)
3. [The Java-like Default Experience](03-java-like-experience.md)
4. [Memory-Safety Threat Model](04-threat-model.md)
5. [How the Analysis Works (provenance + lifetime)](05-how-analysis-works.md)

## Part II — The Problems, One Per Chapter

Each chapter: *the C++ problem → how CeeLing solves it → proof by example.*

10. [Smart-Pointer Inference](10-smart-pointer-inference.md) — who owns what
11. [Dangling References](11-dangling-references.md) — the core guarantee
12. [Use-After-Move](12-use-after-move.md) — the cost of auto-`std::move`
13. [Out-of-Bounds Access](13-bounds-checking.md) — runtime backstop
14. [Null Dereference](14-null-safety.md) — empty smart pointers
15. [Iterator Invalidation](15-iterator-invalidation.md) — the C++-specific trap
16. [Dangling Views: `string_view`, `span`, Eigen](16-views-and-eigen.md)
17. [Sealed Escape Hatches](17-escape-hatches.md) — `.get()`, casts, raw `new`
18. [Implementation Notes](18-implementation-notes.md) — what is actually built, and how

## Part III — Context

6. [Why Not Transpile to Rust](06-why-not-rust.md)
7. [Honest Comparison with Rust](07-vs-rust.md)
8. [Landscape: Rust, Circle, and C++ Profiles](08-landscape.md)
9. [Status & Roadmap](09-status-and-roadmap.md)

## Related docs

- [Smart Pointer Analysis (two-pass algorithm)](../SMART_POINTER_ANALYSIS.md) — merged into Ch. 10
- [Smart Pointer Rules](../smart_pointer_rules.md)
- [Before/After Examples](../before_after_examples.md) — merged into Ch. 10
