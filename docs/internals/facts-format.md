# The `.clfacts` format (version 1)

A fact pack is a plain-text file that tells the compiler what it may assume about a library. This is the format the built-in packs in `facts/` use today; the plan for what
grows out of it (generated facts, downloadable packs, header-hash binding) is in `docs/design/third-party-inference.md`; the status of each part is in the [guide, chapter 18](../language-guide/18-status.md).

## Shape

```
# comment
@pack std
@version 1
@origin hand-written, migrated from library_summary.cpp

vector.method push_back invalidates shares-arg0 source=declared review=hand
function std::stoi source=declared review=hand
```

* **Header lines** `@pack`, `@version`, `@origin` come first and are all required. The pack is reported as `pack@version`.
* **A fact** is one line: `<kind> <subject> [flag ...] source=<declared|inferred> [review=<state>]`. Words are separated by spaces, so a subject has none (`Eigen::MatrixXd|*` for an operator).
* `#` starts a comment line; blank lines are ignored; CRLF is accepted.
* Optional header lines: `@header <path> sha256=<64 hex digits>` (a header the facts were scanned from; repeatable; the path is relative to the pack file) and `@scanner <text>`. **The compiler enforces the hashes** when
  a pack is loaded with `--facts`: every recorded header that exists must hash (SHA-256 of the content with line endings normalised to LF) to the recorded value, otherwise the pack is refused (exit 2) with a message that names the
  pack and the header. A header that cannot be found at the recorded place is a warning on stderr, or an error with `--strict-headers`. Built-in packs have no `@header` lines.
* **`source`** is `declared` (a person wrote it) or `inferred` (a scanner wrote it). **`review`** is free text; the shipped packs use `hand`. Later versions add reviewer names and dates.

## Kinds and flags

| kind | subject | flags | meaning |
|---|---|---|---|
| `vector.sig`, `string.sig`, `string_view.sig`, `optional.sig`, `map.sig` | `name/const\|nonconst/<parameter kinds joined by +>` (`-` for none) | attributes `result`, `mutates`, `partial`, `impls` | what a scan found for one SIGNATURE (below); written by `gen_pack.py`, `source=inferred review=unreviewed` |
| `span.sig` | `name/const\|nonconst/<parameter kinds joined by +>` | attributes as above | the same shape for CeeLing's own `std::span<T>` (`cling::span`, a runtime type of the compiler): written by hand (`source=declared review=hand`), because there is nothing to scan; `result=view` is another span of the same vector, `element-reference` an element |
| `vector.note` | method name | `shares-arg0` | a declared fact no signature can give: the first argument is linked into the container, not consumed |
| `vector.own` | method name | none | a method CeeLing itself defines (`copyWithin`); admitted without a scan fact |
| `function` | called name | none | a free function the program may call; matched on the bare name, with or without a namespace qualifier |
| `class` | type name | none | an external class type that may be declared (`Eigen::MatrixXd`) |
| `operator` | `Type\|op` | none | the operator may be applied to that type; its result is always fresh |
| `borrow` | `Class::method` | none | the method returns a view that borrows the receiver, with real dangling tracking |
| `fresh` | `Class::method` | none | a method of an external class that returns a fresh value (a number, an owned object), borrows nothing, does not change the receiver and **does not retain its arguments** |
| `mutator` | `Class::method` | none | a method of an external class that changes the receiver (its observers die) but returns nothing that borrows and retains none of its arguments |
| `opaque` | type name | none | an external class nobody wrote method facts for: the conservative ("trusted") model, below |

## Signature facts (`*.sig`)

The subject names the signature: the method, its const-ness and the KIND of each parameter: `value` (a plain value or const reference), `args` (a parameter pack), `mref` (a non-const lvalue reference), `iterator`,
`initlist`, `allocator`, `pair`, `ptr-w` (a pointer to writable memory), `cstr` (a `const char *`, how a string literal arrives). Example: `map.sig erase/nonconst/value` is `erase(key)`, `map.sig erase/nonconst/iterator`
is `erase(iterator)`. The facts only say what each parameter IS; the compiler decides which kinds CeeLing can write (not `iterator initlist allocator pair ptr-w`), so no language rule is baked into the data.

`result` is a comma-separated set of the shapes the signature returns: `void`, `scalar`, `string` (a fresh std::string), `self-reference` (`*this`), `element-reference`, `iterator-view`, `view`, `raw-pointer`, `value` (a type
CeeLing has no shape for), `unresolved`. `mutates=yes`: the signature may change the receiver (a non-const member with no const twin), so every observer of the container dies. `partial=yes`: a scanned implementation could not resolve an overload that another resolved. `impls`: the implementations scanned. `result` and `mutates` are required.

**Admission is computed from these, not listed** (`library_summary.cpp`, `methodVerdict`): a name is admitted when, for every CALLABLE signature (no parameter of kind `iterator initlist allocator pair ptr-w`), there is no `mref`
parameter and every shape in `result` is one of `void scalar string self-reference`; the refusal reason for the first shape that is not is part of the error message. A name with no fact, or with no callable
signature, is not admitted. Signatures that cannot be called do not count against the name, which is how `map::erase(key)` is admitted while `erase(iterator)` is not.

## What the loader refuses

An unknown kind, an unknown flag for the kind, a missing `source=`, a `source` other than the two values, a duplicate fact (also across packs), a fact before the header, a missing header line,
an unknown `@` line. A failed load adds nothing. A shipped pack that fails to load aborts the compiler at start-up with the message, rather than running with part of a table. Nothing is ever
skipped silently, because a skipped line turns a "no" into a "yes" without anyone noticing.

## Project packs: `cling --facts mylib.clfacts`

A project teaches the compiler about its own C++ library with a pack of `class`, `opaque`, `fresh`, `borrow`, `operator` and `function` facts (repeatable flag). A `class` fact makes the name a known type (a value type, emitted
bare like `Eigen::MatrixXd`); `fresh` and `borrow` list the methods that may be called on it; a method not listed is rejected as an unknown call. A project pack can only ADD: it may not hold the std signature kinds (so it
cannot relax how `std` is judged) and may not repeat a fact the compiler ships (so it cannot replace one). A malformed or missing pack is an error (exit 2). `tests/facts/private_lib/` is a worked example: a header
(`grid.h`), its facts (`grid.clfacts`), a program that uses it and runs, and the two rejections.

`cling --report-facts` prints, after a successful compile, every fact a check answered "yes" from, with its pack and source: the trusted-boundary report. A fact whose source is `inferred` or whose review is `unreviewed`
is a scanner's draft; a `declared` fact in a project pack is the project's own claim about its library.

## `opaque`: a library nobody wrote facts for (the conservative model)

`opaque Name` declares `Name` as a known value type (like `class Name`) whose methods are ALL callable, and assumes the worst about every method that has no `fresh Name::m` or `borrow Name::m` fact:

* **its result borrows the receiver**: `Grid w = g.m();` makes `w` an observer of `g` (also by assignment, `w = g.m();`), so replacing or moving `g` kills `w`;
* **it changes the receiver**: any call of such a method on `g` kills every existing observer of `g`, wherever the call stands in the statement (nested calls included; a use and a kill in one expression count as kill first);
* **it may retain an object argument**: `g.m(h)` with an object-typed local `h` is refused, because nothing follows a reference the library keeps; a number, a `bool` or a `string` argument is fine;
* the result may borrow an argument too: `Grid w = g.m(h);` is refused (an observer follows one owner).

A `fresh` or `borrow` fact for a method replaces the default for that method; both assert, besides what they say, that the method does not retain its arguments. Not covered, still refused or unmodelled: operators on the type
(they need an `operator` fact), a borrow taken from a FIELD receiver (`Grid w = this_field.m();` is refused for every borrowing method, `opaque` or listed, because a field has no owner the analysis follows), and
anything the library does behind the call: the class's internals are trusted, as for an `ext_fn`. `tests/facts/private_lib/opaque*.clg` and `tests/facts/eigen_trial/` (a real `Eigen::VectorXd` with a two-line pack) are the examples.

## Writing a pack by scanning: `tools/cling-scan`

`python tools/cling-scan/cling_scan.py --clang clang++ --header grid.h --class Grid --out grid.clfacts` writes `opaque Grid` plus the `fresh`, `borrow`, `mutator` and `function` facts the declared signatures prove, with a `#`
comment for every method it could not decide. See `tools/cling-scan/README.md` for exactly what it proves and what it does not.

## Review states and `--strict`

`review=` is free text without spaces, but three shapes mean something:

| `review=` | who vouches | set by |
|---|---|---|
| `unreviewed` | nobody: a scanner wrote it | `cling-scan` |
| `probed:<date>` | a probe ran the fact against the real library (evidence, not review) | `probe_pack.py --mark-probed`, only on a passing, non-pretend run, only for facts it exercised (`fresh`/`borrow`/`mutator`; not `function`) |
| `reviewed:<who>:<date>` | a named person | `tools/cling-scan/review_pack.py --reviewer <name>` (a person runs it; nothing in this repository does it on anyone's behalf) |
| `hand` | the project's author wrote the fact | by hand |

`cling --strict` refuses to compile (exit 3, no output file) a program that RELIED on a fact of a `--facts` pack that is `source=inferred` and not `reviewed:...`. `declared` facts pass (a person wrote them), `opaque` passes always (it can only make
checking stricter), facts the program does not use are irrelevant. `--strict=probed` also accepts `probed:...`. The facts shipped inside the compiler are not audited by `--strict` (the compiler's own gauge and tests answer for them); `--strict-all` audits
them too and fails today, because the scanned `std::vector`/`string`/`map` facts are `inferred/unreviewed`: that is the honest state, and reviewing them is the work a pack author or a maintainer would do.

## What a pack cannot do

Packs are data. Loading one runs nothing and reads no other file. A fact cannot make a sealed construct legal: raw pointers, `.get()`, casts and unions are rejected by `EscapeHatchChecker`
whatever a pack says. (The name lists the compiler still keeps in C++, such as the primitive types, the `cling::` runtime helpers and `Channel`, are not library facts and are not in a pack.)

## How the built-in packs are shipped

`facts/*.clfacts` is turned into a generated source file by `cmake/embed_facts.cmake` and linked into the compiler, so `cling.exe` stays one file with no data directory. Editing a pack
re-runs the generator at the next build. `tests/facts/fact_table_test.cpp` (CTest `cling_fact_table`) covers every load error and checks the shipped packs.

## Not in version 1 yet (planned, see the TODO)

The library version and header SHA-256 a pack was scanned from; the scanner and implementation versions; `--facts <file>`; result-shape and parameter-mode facts; `sealed`
and `not yet` admission states; `trusted include`.
