# Design record: how the compiler learns what a library does

**Status: mostly built.** This page keeps the reasoning and the plan. How the facts work today, and how to use them, is in [guide chapter 11](../language-guide/11-talking-to-cpp.md) (sections "A library nobody wrote facts for" and "Who vouches for a fact"); the file format is in [internals/facts-format.md](../internals/facts-format.md); what is open is in [chapter 18](../language-guide/18-status.md).

Built: facts are data (`facts/*.clfacts`); `std::vector`, `string`, `string_view`, `optional` and `map` are admitted by rule from scans of the real declarations; project packs (`--facts`), the `opaque` tier, `--report-facts`, `--strict`, header hashes, the fact gauge, `cling-scan` (text, libclang and LibTooling backends) and the AddressSanitizer probes; unknown type names are compile errors. Not built: facts derived from library bodies, buffer-size contracts, demand-driven scanning, downloadable packs. Section 7 lists each against the original plan.

## 1. The problem, as it stood when this was written

* The compiler knew a library only through hard-coded C++ tables (the `std::` names, eight Eigen view methods, a few graphics names). A private library was unusable: every call was "unknown function/method", rejected by the default-reject rule. The only door was `ext_fn`, which takes primitives and `std::vector<primitive>`.
* An unknown *type* name was not rejected at all. `auto`, `string_view`, `span`, iterators and any class the compiler had never heard of passed through to the generated C++ unchecked, so a missing fact meant "fine", not "dangerous".
* A fact was tiny (`FunctionSummary { returnBorrowsParam }` plus ad-hoc name lists). There was no place to say "this method may reallocate" or "this type is a view".

So there are two jobs: give the compiler a fact model and a data format that is not C++ source, and produce the facts for libraries nobody wrote a table for. A whitelist is only the first.

## 2. What a fact is

Per **type** (its shape): *value* (owns its storage; `Eigen::MatrixXd`, `std::string`), *view* (refers into another object; `Eigen::Block`, `string_view`, `span`, iterators), *handle* (owns through a pointer; `unique_ptr`-like) or *plain* (trivially copyable).

Per **callable**: its result (fresh, borrows `self`, borrows parameter *i*, borrows a temporary, unknown); each parameter (read-only, retained by the callee, consumed, mutated); whether the call invalidates observers of `self`; and for each of these a source and a review state.

The dangling analysis itself did not change: what the compiler already does with its own classes (summaries, observers, death events) works on exactly these facts. Only where the facts come from is new. The format as built carries the result kinds, the invalidation fact and the source and review state; it does not carry per-parameter modes. A parameter is handled conservatively instead (an object passed to an `opaque` method is refused, because it may be kept).

## 3. The rules the design holds to

1. **Errors go in the conservative direction.** An inferred fact may be too cautious (a safe program is rejected and the user adds a declaration); it must never be too relaxed. "Fresh" (borrows nothing) is inferred only from structural evidence: a primitive, or a by-value return of a type with no pointer or reference member. Everything else defaults to "borrows".
2. **Unknown is not fine.** A type or call without a fact is rejected, with a message that names the missing fact.
3. **Facts are data, never code.** Loading a pack runs nothing and reads no other file.
4. **A fact cannot open a sealed hatch.** Raw pointers, `.get()`, casts and unions are rejected whatever a pack says.
5. **The gauge counts false accepts, and the number must be zero.** For every borrow or invalidation fact, a seeded dangling program generated from it must be rejected. False rejects are measured and reported: they are the price, not a failure.
6. **A fact is only as good as its evidence, and the evidence is visible.** Each fact carries `source` (declared by a person, or inferred by a scanner) and a review state, and `--report-facts` and `--strict` use them.

### The trust tiers

1. **Declared:** a human-written, reviewed pack. Strongest.
2. **Trusted but undeclared:** conservative default model for a library nobody wrote facts for (the result may borrow from the receiver and every argument; arguments may be retained; a non-const method may invalidate every observer). Over-cautious and sound for CeeLing's side. Built as `opaque`; the proposed spelling `trusted include "lib.h";` was not built.
3. **Inferred:** facts a tool derived from the library's own declarations. Used automatically, marked `inferred` and `unreviewed`, listed by the trusted-boundary report, refusable by `--strict`.
4. **Nothing:** rejected.

## 4. Where facts can come from, in order of reliability

| Source | What it gives | What it cannot give |
|---|---|---|
| **A pack written by a person** | anything | needs a person |
| **Lifetime attributes** (`[[clang::lifetimebound]]`, `[[gsl::Owner]]`, `[[gsl::Pointer]]`) | exact "result borrows this argument", owner or view shape | only where the author wrote them. In the standard headers checked here they sit on free utilities (`min`, `max`, `move`, `forward`), not on container members, so they help libraries that adopt them, not `std` or Eigen |
| **Signature structure** (clang's AST, template-instantiated) | return type kind (`T&`, `T*`, view class), `const`, by-value and rvalue parameters, copy-constructibility, class layout | whether the callee *stores* an argument; which calls invalidate |
| **Body analysis** (the library's own inline and template bodies) | stores a parameter; returns a parameter-derived reference; reallocates, which is how invalidation would be *derived* rather than guessed | bodies that are not visible (a precompiled library). **Not built** |
| **Probe evidence** (generated programs under AddressSanitizer) | confirms or refutes a claimed fact on the real library | needs synthesisable arguments; evidence, not proof |

Invalidation is the fact no signature carries. A name list (`push_back`, `resize`, `erase`) is a heuristic that happens to work for `std`. In the built version "invalidates" is derived for the standard containers as "a non-const member with no const twin", and checked against the standard's own list by the gauge. For a private container, deriving "may reallocate" from the body is the missing piece: without it, `a(i, j) = 5` on a matrix would kill every view of `a`, so `opaque` is deliberately blunt.

## 5. Why a real front end, and why a separate tool

* **A real front end.** Eigen showed that signatures cannot be parsed by a scanner: macros, dependent return types, CRTP, members pulled in from `.inc` files in the middle of a class body. Only a compiler front end resolves aliases and instantiations. Doing it ourselves would be a C++ parser, which is the thing CeeLing exists to avoid.
* **Not inside `cling`.** LLVM is large, slow to build and tied to a version, and a user of a pack needs none of it. So `tools/cling-scan` is an optional tool run by whoever produces facts (a library maintainer, a company's build team, CI); everyone else consumes data files. The compiler never links LLVM.
* **Three backends, one analysis.** A text backend over `clang -ast-dump` (the first, and the only one that could read a template instantiation until LibTooling), a libclang backend over the stable C API (classes, not instantiations), and a LibTooling program on clang's AST. They are tested to give the same verdict for every method on the same classes.
* **Witness types.** A template's facts depend on its arguments. `--witness` scans a second instantiation and keeps only the facts on which both agree.

## 6. Decisions that were taken

1. *Is `cling-scan` as a separate, optional LLVM tool the right boundary?* Yes.
2. *Should unknown type names be rejected?* Yes, since 2026-11-02. It was the one step that could break existing programs (`auto`, `string_view` and `span` passed through before); `auto` stays refused, and `string_view` and `span` are tracked.
3. *Which private-library stand-in tests generality?* A private class, jsoncpp's `Json::Value`, yaml-cpp's `YAML::Node` and Eigen's `VectorXd`. The LAPACK-shaped test (section 8) was not built.
4. *Is a trusted tier acceptable before any scanner exists?* Yes; it shipped as `opaque`.

## 7. The original phases, against what exists

| Phase | Plan | Status |
|---|---|---|
| 0 | fact model, format, loader, tier 2, unknown types rejected | done (`opaque` instead of `trusted include`) |
| 1 | `cling-scan`: signature-level facts; the scan agrees with the hand tables on `std`; reproduces the Eigen view methods | done for `vector`, `string`, `string_view`, `optional`, `map` and for single classes; not demand-driven |
| 2 | body-derived facts: retains, returns-param-derived, may-reallocate | **not built** |
| 3 | probe validation per fact | built as `probe_pack.py` (Eigen `VectorXd`, jsoncpp, yaml-cpp); a probed fact is marked `probed:<date>`; a fact without a passing probe stays `inferred` |
| 4 | a second, unrelated library as a generality test | done for the three above; the C-API-with-contracts shape is not built |

## 8. Not built, and why it would be worth building

* **Body-derived facts** (retains an argument, may reallocate). Would let a private container's mutating calls be judged precisely and would make some `opaque` rejections go away. Needs the library's bodies; a precompiled library falls back to the conservative default.
* **Buffer-size contracts (the LAPACK shape).** Numerical C libraries take raw buffers, and the dominant bug is a buffer too small for what a routine writes. A signature gives direction (`const` input, non-`const` output) but not the size contract (`A` is `LDA x N`, `LDA >= max(1, N)`, `WORK` has `LWORK` elements). The idea: a new fact kind, "parameter *p* needs at least `f(other parameters)` elements", with `f` limited to products, sums, `max` and `min`; the compiler then checks a call on a `std::vector<double>` at the call site (statically for constants, otherwise a run-time check that aborts with the line, like `cling::at`). It would also make a mutable container reference safe in an `ext_fn`, since a routine that writes within the buffer cannot reallocate it. Eigen ships a C++ LAPACK subset that could serve as the test. "Does not retain its arguments" cannot be derived from a Fortran body; for such a library it stays a documented convention that a pack states and a reviewer confirms.
* **Demand-driven scanning.** The compiler already knows which foreign names a program uses; instead of rejecting them one by one it could list them (`--fact-demands`, like `--ptr-demands-out`) and `cling-scan` could answer exactly that list. This keeps scans small and makes templates tractable (facts per instantiation, which is what the program uses).
* **Downloadable packs.** Shaped like cargo, and nothing more: a manifest, a lock file with the content hash of every file, packages fetched by a separate tool (`cling fetch-facts`). The compiler never touches the network and reads only local packs, bound to the SHA-256 of the headers they describe (this binding is built). A pack is data, tiers stay visible, `--strict` accepts only declared and reviewed facts. A downloaded scan is as trustworthy as a downloaded C++ library, and no more.
* **A release gate for the built-in `std` pack.** Rule: the pack is generated, never hand-edited, and a release must not go out while the regenerated pack differs from the committed one unreviewed. What exists: `tools/std-scan-experiment/gen_pack.py` and the CTest `cling_facts_generated`, which fails when the committed pack is not exactly what the script produces. What does not: a rescan against newer toolchains in CI, and a changelog check. A stale pack is safe in one direction (a new member is unknown and therefore rejected); the unsafe case is a changed fact about an existing member, which is what a rescan diff exists to catch.
* **`sealed` and `not yet` as admission states** (a policy over the facts, so that a callable returning or taking a raw pointer is sealed by rule instead of being rejected by accident).

## 9. Risks

* A confidently wrong inference is a hole in the guarantee, exactly like a wrong `ext_fn` line. The first Eigen measurement found one: `Unit` and `UnitX` static factories look like "borrows receiver" and really borrow a temporary. The conservative-direction rule, the review step, the probes and the seeded-bug corpus are the defences; none is a proof.
* Facts that live only in documentation (a method invalidates because the manual says so) cannot be inferred. They fall to the conservative default or to a person.
* Version drift: a fact is bound to the library version and the scanner's LLVM version. The header hashes make a mismatch an error instead of a silent pairing.
* Passing CeeLing objects into foreign code stays out of scope: CeeLing has no raw pointer to hand over. The scope is foreign value and view types and the free functions on them.

## 10. A full scan of the standard library: size, keys, review, storage

An early draft limited `std` to the members CeeLing programs used. That is the wrong boundary: it makes the answer to "may I call `X`?" depend on what happened to be written so far. The plan is to scan the library and to separate two questions the hand tables mixed: **facts** (what a callable does: complete, by scanning) and **admission** (may CeeLing call it: allowed, sealed with a reason, or not yet; a small policy over the facts).

* **Size** (a rough one-off measurement, 2026-11, on a translation unit that includes about 50 C++20 standard headers with the clang available then): the AST dump was 1.28 million lines and took about 4 seconds; counting declarations whose names do not start with an underscore gave about 5,300 member functions, 1,800 constructors, 3,700 free functions and 3,900 function templates. An order of magnitude of 10^4 callables, which as a text file is 1 to 2 MB and parses in milliseconds. That count is a regex, not an audit.
* **Keys.** Facts are about templates, but result types depend on arguments. Most facts do not depend on `T` (`vector<T>::begin()` borrows `self` for every `T`) and are stored once per template; some do (`vector<bool>::operator[]` returns a proxy, which is a view). A fact is keyed by template, member and overload, and the scanner instantiates each template with a few witness types and requires equal facts.
* **Review.** Ten thousand facts cannot be read one by one. The unit of review is the rule and its exceptions: the scanner groups callables into categories, the standard's own invalidation rules are transcribed once per category, and a person reads that table plus the callables whose classification disagrees with their category or between two implementations. Cross-checks do the rest (libstdc++ against the MSVC STL; the scan against the hand tables; the probes). The first experiment found the facts identical across the two implementations for 30 of 30 `vector` names and 33 of 46 `string` names, the rest being scanner limits ([REPORT.md](../../tools/std-scan-experiment/REPORT.md)).
* **Storage.** Plain text under version control, one file per header group, one fact per line, sorted, so that review is a pull request. A database would only buy queries across libraries and versions, which is a tool concern buildable later as an index generated from the text. It should not be the source of truth, and the compiler should not depend on it.
* **Where the effort goes.** Template and specialisation handling in the scanner; overload sets; `constexpr` and `noexcept` noise; the C++20 ranges machinery (views that borrow their base, `ranges::dangling`); and I/O and concurrency types whose facts are about state, not borrowing. Those can be scanned and left as "not yet" until someone needs them.

A pack may describe a call; it cannot make a pointer legal.
