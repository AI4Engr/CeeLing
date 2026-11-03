# The `.clfacts` format (version 1)

A fact pack is a plain-text file that tells the compiler what it may assume about a library. This is the format the built-in packs in `facts/` use today; the plan for what
grows out of it (generated facts, downloadable packs, header-hash binding) is in `PLAN_third_party_inference.md`, the work list in `TODO.md` ("Third-party library facts").

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
* **`source`** is `declared` (a person wrote it) or `inferred` (a scanner wrote it). **`review`** is free text; the shipped packs use `hand`. Later versions add reviewer names and dates.

## Kinds and flags

| kind | subject | flags | meaning |
|---|---|---|---|
| `vector.sig`, `string.sig`, `map.sig` | `name/const\|nonconst/<parameter kinds joined by +>` (`-` for none) | attributes `result`, `mutates`, `partial`, `impls` | what a scan found for one SIGNATURE (below); written by `gen_pack.py`, `source=inferred review=unreviewed` |
| `vector.note` | method name | `shares-arg0` | a declared fact no signature can give: the first argument is linked into the container, not consumed |
| `vector.own` | method name | none | a method CeeLing itself defines (`copyWithin`); admitted without a scan fact |
| `function` | called name | none | a free function the program may call; matched on the bare name, with or without a namespace qualifier |
| `class` | type name | none | an external class type that may be declared (`Eigen::MatrixXd`) |
| `operator` | `Type\|op` | none | the operator may be applied to that type; its result is always fresh |
| `borrow` | `Class::method` | none | the method returns a view that borrows the receiver, with real dangling tracking |

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

## What a pack cannot do

Packs are data. Loading one runs nothing and reads no other file. A fact cannot make a sealed construct legal: raw pointers, `.get()`, casts and unions are rejected by `EscapeHatchChecker`
whatever a pack says. (The name lists the compiler still keeps in C++, such as the primitive types, the `cling::` runtime helpers and `Channel`, are not library facts and are not in a pack.)

## How the built-in packs are shipped

`facts/*.clfacts` is turned into a generated source file by `cmake/embed_facts.cmake` and linked into the compiler, so `cling.exe` stays one file with no data directory. Editing a pack
re-runs the generator at the next build. `tests/facts/fact_table_test.cpp` (CTest `cling_fact_table`) covers every load error and checks the shipped packs.

## Not in version 1 yet (planned, see the TODO)

The library version and header SHA-256 a pack was scanned from; the scanner and implementation versions; `--facts <file>`; result-shape and parameter-mode facts; `sealed`
and `not yet` admission states; `trusted include`.
