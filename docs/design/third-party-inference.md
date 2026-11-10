# Design: inferring the safety facts of third-party C++ libraries

> **Status: mostly implemented; this is the design record, not the current description.** Section 1 describes the compiler as it was before the work started (hard-coded
> tables, unknown type names passing through): that is no longer true. What exists now: facts are data (`facts/*.clfacts`, [format](../internals/facts-format.md)); `std::vector`,
> `string`, `string_view`, `optional`, `map` are admitted by rule from scans of the real library declarations; project packs (`--facts`), the `opaque` tier, `--report-facts`, `--strict`,
> header hashes, the fact gauge, `cling-scan` (text, libclang and LibTooling backends) and the AddressSanitizer probes are built and tested; unknown type names are errors. What is not built:
> downloadable packs with a lock file (section 13) and the LAPACK test (section 11). Read the sections for the reasoning behind these choices; for behaviour, read the Language Guide
> ([chapter 11](../language-guide/11-talking-to-cpp.md), [chapter 18](../language-guide/18-status.md)) and the code.

## 1. The problem, stated by what the compiler does today

* The compiler knows a third-party library only through **hard-coded C++ tables** in `library_summary.cpp`: the `std::` names, eight Eigen view methods, a few old `gfx_*` names. A private
  library is therefore *unusable*: every call to it is "unknown function/method", which is rejected (`default-reject`). The only door is `ext_fn`, which takes primitives and
  `std::vector<primitive>` only.
* The other side of the same gap is worse, and the guide found it: **an unknown *type* name is not rejected at all.** `auto`, `std::string_view`, `std::span`, iterators and any class name
  the compiler has never heard of pass through to the generated C++ unchecked. So the facts we do *not* have are not defaulted to "dangerous"; they are silently treated as "fine".
  This is why threat rows #4, #5 and #6 are open.
* The shape of a fact is tiny: `FunctionSummary { returnBorrowsParam }`, plus ad-hoc lists (vector mutators, string methods). There is no place to put "this method may reallocate", "this
  parameter is retained", "this type is a view".

So there are two jobs: (a) give the compiler a **fact model and a data format** that is not C++ source, and (b) **produce the facts** for libraries nobody hand-wrote a table for.
A whitelist is only (a); inference is (b).

## 2. What a fact is (the model the compiler must be able to hold)

Per **type** (the "shape"):

| Shape | Meaning | Examples |
|---|---|---|
| value | owns its storage, copy is deep | `Eigen::MatrixXd`, `std::string`, `std::vector<T>` |
| view | does not own; refers into another object | `Eigen::Block`, `std::string_view`, `std::span`, iterators, `Map<>` |
| handle | owns through a pointer, move-only or shared | `unique_ptr`-like classes |
| plain | trivially copyable, no pointers | `int`, small PODs |

Per **callable** (function, method, constructor, operator):

* **result**: fresh (owns nothing alive) | borrows `self` | borrows parameter *i* | borrows a temporary | unknown;
* **each parameter**: read-only | retained by the callee (stored; the argument must outlive the callee's object) | consumed (moved from) | mutated;
* **receiver**: does the call invalidate observers of `self` (none | iterators and references | everything);
* a **confidence** and a **source** for each of the above (section 4).

Everything the compiler already does with its own classes (`deriveSummaries`, `observerOf_`, death events) works on exactly these facts; only the *source* of the facts is new. That is the
reason the plan does not touch the dangling analysis itself.

## 3. The trust model (three tiers, with one change from the earlier plan)

1. **Declared**: a human-reviewed pack. Strongest.
2. **Trusted but undeclared**: the user writes `trusted include "corp/lib.h";`. Calls are checked under a **conservative default model**: the result may borrow from *all* arguments, arguments may be
   retained (so they count as moved), a non-const method may invalidate every observer of its receiver. Over-cautious, sound for *our* side.
3. **Neither**: rejected, with a message offering both fixes.

The change this plan proposes: **tier 2 becomes tier 2 + inferred.** Between "nothing known" (conservative default) and "a human reviewed it" (declared) there is a third state,
**inferred**: facts a tool derived from the library's own headers and bodies. They are used automatically, are marked `inferred` in the facts and in the trusted-boundary report, and a strict
build can refuse them. This is what makes a private library usable on day one without anyone writing a table, while keeping the claim honest ("excluding what trusted libraries do with what
they are given").

The one rule that keeps inference safe: **errors must go in the conservative direction.** An inferred fact may be too cautious (a safe program is rejected, the user adds a declaration) but
must never be too relaxed. Concretely, "fresh" (borrows nothing) may only be inferred from structural evidence (a primitive, or a by-value return of a plain/value type with no pointer or
reference member); everything else defaults to "borrows".

## 4. Where facts can come from, in order of reliability

| Source | What it gives | What it cannot give |
|---|---|---|
| **Declaration in a pack** (human) | anything | needs a person |
| **Lifetime attributes** (`[[clang::lifetimebound]]`, `[[gsl::Owner]]`, `[[gsl::Pointer]]`) | exact "result borrows this argument", owner/view shape | only where the library author wrote them. **Measured here:** with the clang available on this machine, the standard headers carry `lifetimebound` on 74 declarations, all free utilities (`min`, `max`, `move`, `forward`, `as_const`, `addressof`, `basic_string_view::operator=`), and none on container members. So attributes are a bonus for libraries that adopt them (a private library's owners can add them in minutes), not a source for `std` or Eigen |
| **Signature structure** (clang AST, template-instantiated) | return type kind (`T&`, `T*`, view class), `const` and ref-qualifiers, by-value / rvalue parameters (sinks), copy-constructibility (value vs handle), class layout (pointer or reference members => view) | whether the callee *stores* an argument; which operations invalidate. Measured on `Eigen::MatrixXd` (TODO): 374 of 511 signatures got a real return type with no unclassified left, 55% of them views |
| **Body analysis** (clang AST of the library's own inline/template bodies, when source is available) | stores a parameter into a member or global; returns a parameter-derived reference; **reallocates** (calls a deallocator, assigns an owning pointer member, transitively) which is how invalidation is *derived* instead of guessed | bodies that are not visible (a precompiled library: fall back to the conservative default) |
| **Probe evidence** (generated programs run under ASan) | confirms or refutes a claimed fact on real code: take a view, mutate/destroy the source, read the view | needs synthesisable arguments; evidence, not proof |

Invalidation deserves its own note because it is the fact signatures never carry. A name list (`push_back`, `resize`, `erase` ...) is a heuristic that happens to work for `std`. For a
private container the plan is to *derive* "may reallocate" from the body (second row from the bottom) and to treat a non-const member of an *owner* type as invalidating only when the
derivation cannot rule it out. Without that, `a(i, j) = 5` on a matrix would kill every view of `a`, which would make numeric code unusable.

## 5. Why LLVM/clang, and why as a separate tool

* **Why a real front end at all.** The Eigen plan already showed that signatures cannot be parsed by a scanner: macros, dependent return types, CRTP, members pulled in from `.inc` files in the
  middle of a class. Only a compiler front end resolves aliases and instantiations (`CwiseAbsReturnType` to the true type). Doing it ourselves would be a C++ parser, which is the thing
  CeeLing exists to avoid.
* **Why not inside `cling`.** LLVM is hundreds of MB, a long build and a version-pinning problem, against a compiler that today builds with a small toolchain; and the user of a pack
  needs none of it. So: an **optional tool, `cling-scan`**, run by whoever *produces* facts (a library maintainer, a company's build team, CI); everyone else consumes the data files.
  The compiler never links LLVM.
* **How it reads a private library.** Through `compile_commands.json` (the library's own include paths, defines and standard), so it parses exactly what the project compiles. That is the
  property a hand-written whitelist can never have.
* **LibTooling or JSON AST.** The prototype (`tools/cling-scan-prototype`) used `-ast-dump=json` (about 1 GB for Eigen) plus `decltype` probes. It proved feasibility, but a product should
  use **LibTooling with AST matchers** in C++ (no 1 GB intermediate; direct access to template instantiations, attributes and the CFG for body analysis), pinned to one LLVM release. pip's
  `libclang` 18 could not parse GCC 16's headers; the clang that did (emsdk's) appeared to use the MSVC standard library on this machine (the std declarations it printed use MSVC's naming), so the include path and the standard library of the scan must
  be set explicitly per project.
* **Demand-driven.** Do not scan a library. The compiler already knows which foreign names a program uses; today it rejects them one by one. It should instead **list them**
  (`--fact-demands out.txt`, the same pattern as `--ptr-demands-out`): "Eigen::MatrixXd::block(int,int,int,int)", "corp::Queue<Job>::pop()", with the concrete template arguments. `cling-scan` answers
  exactly that list. This keeps scans small, makes templates tractable (facts are per instantiation, which is what the program uses), and turns the current error message into a work list.
* **What clang's own work may give for free.** Recent clang releases are adding a lifetime-safety analysis. Not verified here; before any body-analysis code is written, check what it already
  exports, because duplicating it would be wasted effort.

## 6. How the compiler consumes facts

* A **facts file** (`.clfacts`, plain text or JSON, versioned) per library and library version: shapes, callable facts, each with source and confidence. Keyed by normalised
  signature with concrete types, not by a mangled name, so a diff is readable in review.
* Loaded by `cling --facts eigen.clfacts` or from a project file (`cling.toml`, planned as the first layer). The hard-coded tables in `library_summary.cpp` move out into
  shipped packs (`std`, `Eigen`), generated by the scanner and reviewed once: this is the "declaration tables as data" item (layer 4).
* **Packs can be fetched** (section 13): the compiler reads only local pack files, bound to the hashes of the headers they describe; a separate `cling fetch-facts` command downloads and
  locks them.
* **Unknown types stop being silent.** A declaration whose type has no shape (no pack, no `trusted include`) is rejected, including `auto` when its initialiser's result has no known shape. This
  closes the gap in section 1 and is a prerequisite: inference is pointless while unknown things are accepted.
* `auto x = a.block(...)` becomes *allowed*, not forbidden: the fact says the result is a view borrowing `self`, so `x` is an observer. That turns today's blanket hole (any `auto`) into a
  checked feature.
* Outputs: the **trusted-boundary report** (every call that relied on a fact below "declared", and the fact's source) and a **strict** mode that refuses inferred and tier-2 facts.

## 7. Phases, each with a measurable end

0. **Fact model, format, loader, tier 2, unknown types rejected.** No LLVM. Port the existing hard-coded tables into data files with no behaviour change (all 308 fixtures and the guide
   stay green). Add `trusted include` with the conservative default and the boundary report. Flip unknown type names to default-reject. *Done when:* a private C++ class can be used from
   CeeLing under the conservative model, and `auto it = v.begin()` is a compile error with a message that names the missing fact. (Days.)
1. **`cling-scan` v1: signature-level facts, demand-driven.** LibTooling, template instantiation from the demand list, shapes and result/parameter facts from structure and attributes.
   *Done when:* on `std::vector`, `std::string`, `std::string_view`, `std::optional` the scanner's draft **agrees with the hand tables on every entry** (free ground truth), and on
   `Eigen::MatrixXd` it reproduces the eight hand-listed view methods and finds the rest of the view family; every disagreement is explained. (1 to 2 weeks.)
2. **Body-derived facts: retains, returns-param-derived, may-reallocate.** *Done when:* the invalidation set derived from the library's own bodies equals the hand-written
   `vectorMethodInvalidatesReferences` for `std::vector` (including that `reserve` does not invalidate), and `a(i,j) = x` on a matrix invalidates nothing. (Weeks; the largest unknown.)
3. **Probe validation.** Generate, per fact, an accepted and a rejected CeeLing program plus an ASan probe; a fact without a passing pair stays `inferred` and cannot be promoted to declared.
   (Weeks.)
4. **A second, unrelated library** as the test of generality: not Eigen and not `std`: a small real C++ library (a JSON, a graphics or a container library of the kind a company would
   have). *Done when* a new CeeLing program using it needs no hand-written line, and a seeded-bug corpus is fully rejected (below).

**The gauge for "is inference good enough"** (to be built in phase 1 and run in every later phase): for every inferred borrow or invalidation fact, a *seeded dangling program* generated from
it must be **rejected** (false accepts must be zero), and a corpus of *valid* usage (Eigen's own examples, `std` usage in the existing projects) must be **accepted** at a reported rate (false
rejects are measured, not hidden). False accepts are the failure that breaks the contract; false rejects are only the price.

## 8. Risks and what is not solved

* **A confidently wrong inference is a hole in the guarantee**, exactly like a wrong `ext_fn` line. TODO's Eigen measurement already found one: `Unit`/`UnitX` static factories look like
  "borrows receiver" and are really "borrows a temporary". The conservative-direction rule, the review step and the seeded-bug corpus are the defences; none is a proof.
* **Facts that live only in documentation** (a method invalidates because the manual says so, with no code path to derive it from) cannot be inferred. They fall to the conservative default or to
  a human declaration.
* **Opaque libraries** (binary only, headers declare, bodies unavailable) get signature facts only; "retains" and "reallocates" default to yes.
* **Version drift**: a fact is bound to the library version and the scanner's LLVM version; a new release re-runs the scan and shows the diff.
* **Passing CeeLing objects into foreign code** stays out of scope: CeeLing has no raw pointer to hand over. The scope of this plan is foreign **value and view types and the free functions on
  them** (Eigen, `string_view`, spans, iterators, containers, a private library's value types), which is also what `ext_fn` cannot express.
* **Effort and certainty.** Phase 0 is certain and small. Phase 1 is well supported by the prototype. Phases 2 and 3 are research-grade: I would not promise them before phase 1 shows
  how much of a real private library the signature facts already settle.

## 9. Decisions needed

1. Is `cling-scan` as a **separate, optional LLVM tool** the right boundary (recommended), or should any LLVM be allowed into the compiler build?
2. Should **unknown type names be rejected** (breaking: programs that today rely on pass-through, such as `auto` and `string_view`, stop compiling until facts exist)? Recommended, but it is the
   one step that can break existing code.
3. Which **private-library stand-in** do we use for phase 4? Candidates: one of our own sibling projects' dependencies (webview's C API is too thin), or a small open-source C++ library of the
   kind companies write internally.
4. Is `trusted include` (tier 2) acceptable as the first shipped step, before any scanner exists?

## 10. The standard library: scan all of it (revised after discussion)

An earlier draft limited `std` to the members CeeLing programs use today (about a dozen in the sibling projects). That is the wrong boundary: it makes the answer to "may I call `X`?" depend on what
we happened to write so far, and it leaves the sealed-hatch list incomplete. The plan is a **full scan of the C++20 standard library**, separating two questions that the hand tables mixed:

* **Facts**: what a callable does (borrows, retains, invalidates, shape). Complete, by scanning.
* **Admission**: may CeeLing code call it? `allowed` | `sealed` (with the reason) | `not yet`. A policy over the facts, small and reviewable.

A full scan is what makes the sealed set complete *by rule*: any callable that returns a raw pointer or takes one (`data()`, `c_str()`, `get()`, `release()`, `operator&`, C-style buffers) is
`sealed` automatically, instead of being "unknown" and rejected by accident.

**How big is it?** Measured, roughly, on a translation unit that includes about 50 standard headers (C++20) with the clang available here: the AST dump is 1.28 million lines and takes **about
4 seconds**; counting declarations whose names do not start with an underscore (the library's public surface; a rough regex count, not an audit) gives about **5,300 member functions, 1,800
constructors, 3,700 free functions and 3,900 function templates**, in a few hundred public class templates. Order of magnitude: **10^4 callables**. As a facts file at roughly 100 bytes each that is
1 to 2 MB of text; parsing it takes milliseconds.

**Is a database needed?** Not for size and not for lookup. The cost of a full std pack is not storage, it is three other things:

1. **Keys.** Facts are about *templates*, but result types depend on the arguments. Most facts do not depend on `T` (`vector<T>::begin()` borrows `self` for every `T`) and are stored once per template. Some do
   (`vector<bool>::operator[]` returns a proxy, which is a *view*, unlike every other `vector<T>`). So a fact is keyed by template, member and overload, with an optional condition for specialisations. The
   scanner instantiates each template with two or three **witness types** (a trivial type, a non-trivial/move-only type, `bool` where it matters) and requires equal facts across them; a difference becomes a
   conditional fact or a review item.
2. **Review.** Ten thousand facts cannot be read one by one. The unit of review is the **rule and the exception**: the scanner groups callables into categories (sequence, associative, unordered, string,
   view, algorithm, smart pointer, callable wrapper, I/O, ...), the standard's own invalidation rules are transcribed once per category (a dozen rows, section 4), and a person reads (a) the rule table
   and (b) the callables whose classification disagrees with their category, with their attributes, or between the two implementations. Automated cross-checks do the rest: libstdc++ against the MSVC STL
   (both parseable with the toolchains on this machine), the scan against the existing hand tables, and phase 3's sanitizer probes on the callables that return views or invalidate.
3. **Drift.** A new standard-library release changes some facts silently. The pack records the library and compiler versions; a rescan prints the diff; the diff is what gets reviewed.

So the pack is **plain text under version control**: one file per header group (`vector.clfacts`, `string.clfacts`, ...), one fact per line, sorted, so that git shows a readable diff and review is a
pull request. The compiler loads only the files for the headers a program includes. A database (SQLite, say) would buy queries across libraries and versions ("which facts did reviewer X confirm?",
"which inferred facts are still unreviewed?"); that is a *tool* concern, buildable later as an index generated from the text files. It should not be the source of truth, and the compiler should not
depend on it.

**Where the effort really goes** (so the plan does not understate it): template and specialisation handling in the scanner; the overload sets (`operator[]`, `at`, `insert` have many); `constexpr` and
`noexcept` noise in signatures; the C++20 ranges/views machinery (views that borrow their base, range adaptors, `ranges::dangling`); and I/O and concurrency types whose facts are about state, not
borrowing. Those can be scanned and marked `not yet` for admission until someone needs them.

What facts must **never** do: open a sealed hatch. A pack may describe a call; it cannot make a pointer legal.

## 11. LAPACK as the generality test (phase 4)

LAPACK is a good choice precisely because it is **not** Eigen-shaped, and it is the shape of the private numerical libraries companies actually have.

| | Eigen / std | LAPACK |
|---|---|---|
| Interface | C++ classes, templates, views | C or Fortran-ABI functions on raw buffers |
| Main danger | dangling views, invalidation | **a buffer too small for what the routine writes** (heap corruption inside the library) |
| Facts a signature gives | result kind, `const` | `const` on a pointer parameter = input; non-`const` = output or in/out |
| Facts a signature does not give | invalidation | the **size contract**: `A` is `LDA x N`, `LDA >= max(1,N)`, `IPIV` has `N`, `WORK` has `LWORK`, `LWORK >= ...` (workspace query with `LWORK = -1`) |

What this exercises that Eigen does not:

* **A new fact kind: buffer contracts.** "parameter *p* must have at least `f(other parameters)` elements", with `f` limited to products, sums, `max`, `min` of the parameters. With it the compiler can
  check a call on a `std::vector<double>` *at the call site* (statically when the sizes are constants, otherwise a run-time check that aborts with the line, in the style of `cling::at`). That closes
  the dominant bug class of numerical C libraries at the boundary, which `ext_fn` cannot do today (it passes a vector as a read-only `const&` or not at all; a mutable container reference is refused
  because it could reallocate. A routine that writes *within* the buffer cannot reallocate, so the contract is what makes the mutable form safe).
* **Facts from documentation, mechanically.** Reference LAPACK documents every argument in a fixed comment grammar (`\param[in]`, `\param[out]`, `\param[in,out]`, `dimension (LDA,N)`,
  `LDA >= max(1,N)`). The scanner reads headers for direction and `const`, and the comment blocks for dimensions: a documentation-derived source with a lower confidence than a body proof, labelled as
  such.
* **"Does not retain its arguments" cannot be derived from a Fortran body** (clang does not parse Fortran). For LAPACK this is a documented convention, which a pack states and a reviewer confirms.
  This is the honest limit of inference for an opaque library, and the test shows it plainly rather than hiding it.

Decision (2026-11-01): use the **C++ LAPACK that ships with Eigen**, nothing is installed. Material on this machine: no reference LAPACK is installed, but **Eigen ships one**: `C:\project\eigen\lapack\` has the header with the Fortran-ABI prototypes (`const int *`, `const double *`), the C++
implementations of a subset of routines (`.inc`, which also lets the body-analysis phase run on real library code) and some reference Fortran files with the standard comment blocks. A full
reference LAPACK/LAPACKE or OpenBLAS would need installing (for example the MSYS2 packages), which I have not done.

*Done when:* a CeeLing program calls `dgesv`, `dgetrf`, `dpotrf` and `dgemm` (or the subset Eigen's package provides) with **correct** buffers and gets the same numbers as Eigen's own solver; with a
buffer one element too small the call is **rejected or aborts at the call site with the line**, instead of writing out of bounds; and no hand-written fact line was needed beyond the pack review. Eigen
stays the test for views and invalidation; LAPACK is the test for the C-API-with-contracts shape.

## 12. Keeping the built-in `std` pack current (decided 2026-11-02)

The `std` pack ships inside the compiler release; users cannot be asked to rescan. The rule is: **the pack is generated, never hand-edited; every release regenerates it; a release cannot be
cut while the regenerated pack differs from the committed one in an unreviewed way.**

* **Why a stale pack is safe in one direction.** A new standard-library version mostly *adds* members and overloads. A member the pack does not know is rejected (default-reject), so a stale
  pack is too restrictive, not unsafe. The unsafe case is a fact about an *existing* member that changed (a function that used to return a copy now returns a view, a call that now
  invalidates). Rare, but it is exactly what the rescan diff exists to catch.
* **Implementation-independent facts.** The scan runs on every supported implementation (libstdc++, libc++, MSVC STL; this machine has two of them). A fact is the **conservative union**:
  borrows if any implementation borrows, invalidates if any invalidates, "not yet" if the implementations disagree about shape. So the pack does not depend on which one the user's g++/MSVC
  uses. The REPORT of the first experiment found 30 of 30 vector names and 33 of 46 string names identical across two; the rest were scanner limits, not real differences.
* **Release gate** (`tools/release_packs.sh`): rescan with the pinned toolchains, write the pack, diff against the committed pack. New members: added as `not yet` automatically (safe).
  A changed or removed fact for an existing member: the release fails until a person has looked at the diff and the change is recorded in the pack's changelog. The pack carries the
  implementation versions it was scanned with and a content hash that `cling --version` prints.
* **Between releases:** a scheduled CI job (weekly) rescans against the newest released toolchains and opens an issue with the diff when it is not empty. Cost is seconds per toolchain.
* **What the user sees:** `cling --report` lists the pack versions in use. A project that wants a newer `std` than the release knows keeps working (new members rejected with a message that
  names the missing fact and how to supply it, see phase 0); a project can add a local `--facts` file and the boundary report shows it.
* **Not solved:** a user compiling with a standard library we never scanned (a vendor fork, an old release) gets the union of the ones we did; a fact that differs there is invisible to us.
  Said in the pack header and in the guide, not hidden.

## 13. Downloadable fact packs (decided 2026-11-02)

Rust's cargo shows the shape that works: a small manifest, a lock file with content hashes, packages fetched by a separate tool. We take that shape for **fact packs** (a few text files
about one library version), and nothing more: no registry, no solver, no mirrors. A scan done once by anyone can then be reused by everyone. Reviewed, versioned packs become concrete.

* **A pack is data, never code.** `facts/*.clfacts`, `tests/`, the raw scanner output and the human diff, `PACK.toml`. The compiler never runs anything from a pack and never touches the
  network; `cling fetch-facts` does the download and writes `cling.lock` with the hash of every file. A changed download is an error.
* **Bound to the library by header hash.** The pack records the SHA-256 of the headers it was scanned from; the compiler hashes the headers a program really includes and refuses a pack that
  does not match, saying which header differs. A library upgrade can therefore never be paired silently with last year's facts (the failure mode in section 8, "version drift").
* **Provenance is visible.** Tiers: `declared`, `reviewed` (reviewer named, pack tests pass), `third-party inferred` (downloaded scan, unreviewed), `inferred` (scanned locally), `trusted
  include`. `--report` lists every call that relied on a fact below `declared`; `--strict` accepts only `declared` and `reviewed`.
* **Scanning is cheap to repeat, trusting is not.** `cling verify-pack` reruns the pack's tests, re-hashes headers and rescans when `cling-scan` is available, so a pack's claims can be checked by
  anyone who is willing to build the scanner. A downloaded scan is as trustworthy as a downloaded C++ library and no more.
* **Packs cannot open sealed hatches.** The loader rejects a fact that would make a raw pointer, `.get()`, a cast or a union legal, from any source.
* **The built-in `std` pack uses the same format and loader**, so there is one code path to get right and the std pack is just the first pack.
* **Cost now vs later.** What must be in the first version of the format is cheap: header hashes, per-fact source and review state, data-only. Everything else (fetch command, lock file, verify)
  can follow after phase 1, when a first external pack exists to exercise it.
