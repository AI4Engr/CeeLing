# 6. Why Not Transpile to Rust

An early idea was to transpile CeeLing → Rust internally and use `rustc`'s borrow
checker as a soundness oracle. **This was rejected.** The decision is *structural*,
not a matter of taste, and rests on three layers of argument.

## Layer 1 — Ecosystem

CeeLing must support opaque third-party C++ libraries (**`std`, Eigen**). Rust's
borrow checker can only analyze well-formed Rust, and **there is no Rust equivalent**
for Eigen's expression-template / view semantics. Code that touches Eigen simply
cannot be expressed as Rust for `rustc` to check.

## Layer 2 — Information

Dangling analysis **does not need a library's internals** — it only needs each
function's **borrow signature** (which params are borrowed, what the return value
borrows from, which params are moved). Rust would need *exactly the same* hand-written
signatures for opaque C++ libraries. So:

> Rust covers **no more** than a self-built analyzer, while adding two translation
> layers and error-mapping from Rust's `'a` / `E0499` diagnostics back to `.clg`
> source lines.

Coverage of Rust ⊆ coverage of self-built, and Rust costs more. Net loss.

## Layer 3 — Threats

Half of the threat model is **C++-specific and untranslatable to Rust**:

- **#4 iterator invalidation**
- **#5 `string_view` / `span`**
- **#6 Eigen expression templates**
- **#14 `.get()` back door**

Rust has no corresponding constructs, so it cannot even *see* these — untranslatable
means uncheckable.

## The deeper reason: Rust is a bundle

The self-built checker is the **liveness half** of a borrow checker
(see [Chapter 5](05-how-analysis-works.md)). Rust sells liveness and
**aliasing/mutability** (`&mut` exclusivity) as an inseparable bundle:

- To borrow `rustc`'s liveness analysis you are **forced** to accept its aliasing
  rules. But CeeLing *allows* multiple aliases — a large amount of code that is legal
  in CeeLing would be **rejected by Rust's aliasing rules (false positives)**, because
  Rust forbids what CeeLing permits.
- Meanwhile the C++-specific threats above still cannot be translated.

> **Rust gives you more than you want (imposed aliasing rules → false positives) and
> less than you need (C++-specific threats untranslatable). Both ends are wrong.**

## Conclusion

The analysis is self-built and runs directly on the CeeLing AST. **Rust is not in the
pipeline.** Writing the liveness half ourselves is the *minimal* implementation that
precisely matches CeeLing's needs — smaller than a full borrow checker by an order of
magnitude, and legitimately so, because "single-thread + summary table" removes the
aliasing half's main motivation (data races).
