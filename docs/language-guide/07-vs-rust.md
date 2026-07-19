# 7. Honest Comparison with Rust

Even with the full threat model implemented, CeeLing is **not** "Rust with C++
libraries." There are real, permanent gaps. Stating them honestly bounds the safety
promise and prevents the project from believing its own marketing.

## What CeeLing gives you

A **single-threaded memory-safe subset of modern C++** — dramatically safer than raw
C++, and able to reuse the C++ ecosystem (Eigen, `std`, ...). That is a real,
valuable, and non-imitable position: **Rust cannot offer Eigen.**

## Where CeeLing falls short of Rust

### 1. Data-race safety (Send/Sync) — deliberately abandoned

This is the *other half* of Rust's safety ("fearless concurrency") and its biggest
differentiator. It is **out of scope for v1**. Consequence: **under multithreading the
memory-safety promise does not hold, and the compiler will not stop you.** This is the
largest gap, and it is by design.

### 2. Aliasing guarantees — not attempted

Rust's `&mut` exclusivity gives (a) aggressive `noalias` optimization and (b)
prevention of mutable-aliasing logic bugs. CeeLing allows multiple aliases and gets
neither. Not fatal to memory safety (liveness checks cover dangling), but it is part
of Rust's *correctness* story that CeeLing lacks.

### 3. Maturity / soundness — an engineering gap, but real

`rustc`'s borrow checker is *trusted*: rarely unsound, validated over a decade
(Polonius / RustBelt formalization). CeeLing's self-built analyzer — lambda escape
(#7), temporary binding (#8), cross-file summary correctness — **each may contain
implementation holes, and one hole is one UB escape route.** For a long time CeeLing's
"99.99%" is **a goal we work toward, not a verified fact.**

### 4. Ecosystem safety — C++ libraries were not built for it

CeeLing reuses `std`/Eigen, but those libraries are **full of UB-capable interfaces**
(`.data()`, iterators, `c_str()`, internal reinterprets). The allow-list summary table
holds the line, but **CeeLing's safety is bounded by how many library summaries were
written correctly.** Rust's ecosystem is safe by default; CeeLing's is safe by
curation.

### 5. Move semantics — patch vs. root fix

Rust move makes the source **unusable at compile time** (ownership transfer). C++ move
leaves a **valid-but-unspecified empty shell** that is still accessible (→
use-after-move). CeeLing simulates Rust's effect with static use-after-move checking
(#10), but that is an *after-the-fact patch*, not a type-system guarantee.

## Bottom line

> Implementing dangling / bounds / null / use-after-move / sealed hatches yields a
> **single-threaded memory-safe modern-C++ subset** — far stronger than raw C++, and
> genuinely useful. Versus Rust the gaps are three kinds: **by design** (data races,
> aliasing), **by maturity** (self-built analyzer vs. a decade-validated `rustc`), and
> **by ecosystem** (C++ libraries are unsafe by nature vs. Rust's safe-by-default
> crates).

CeeLing is **not** catching up to Rust. It is **trading a bounded amount of safety for
the C++ ecosystem** — a real position for people who need Eigen *and* more safety than
raw C++.
