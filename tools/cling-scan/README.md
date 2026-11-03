# cling-scan (prototype)

Writes a draft fact pack for one C++ class from its declaration, so a project does not have to type the facts of its library by hand.

```
python -I tools/cling-scan/cling_scan.py --clang <clang++> --header grid.h --class Grid [-I include-dir ...] --out grid.clfacts
cling --facts grid.clfacts program.clg
python -I tools/cling-scan/cling_scan.py --clang <clang++> --check grid.clfacts      # is the header still the one the pack was scanned from?
```

## What it writes

For every PUBLIC method of the class it reads the declared signature (never the body) and writes a fact only when the signature PROVES it:

| fact | when |
|---|---|
| `fresh Cls::m` | `const`, returns `void`, a number, `bool`, an enum, a `std::string`, or a `std::vector` of those. Nothing it returns can refer to the receiver, calling it changes nothing. |
| `borrow Cls::m` | `const`, returns a reference, a pointer, a `string_view`/`span` or an iterator: the result refers to the receiver. |
| `mutator Cls::m` | not `const`, returns `void`, a number, a string...: it changes the receiver (so observers of it die) and returns nothing that borrows. |
| `function Cls::m` | `static`, returns a number, `bool`, `string`. |

plus `opaque Cls` first, so every method WITHOUT a fact keeps the conservative default (its result borrows the receiver, calling it kills the receiver's observers, an object argument is refused). When it cannot prove a
verdict it writes a `#` comment with the reason and no fact: a class returned by value ("the type alone does not say whether it refers to the receiver"), a non-const method returning a reference, overloads that disagree,
member templates, operators (write `operator` facts by hand). Every fact is `source=inferred review=unreviewed`; the pack header records the header's path and `sha256=` (`@header`), shown by `cling --report-facts`.

## What it does not do (read before trusting a pack)

* It reads **signatures**. `const` is taken at its word (a `mutable` member, a global or a pointer the class keeps can make a `const` method not fresh), and "returns a number" is the declared type. A scanner cannot see a method
  that returns an `int` which indexes the receiver's internals. The conservative direction is built in (anything it cannot prove stays opaque), but a **confidently wrong** inference is possible and is the same kind of hole as a wrong `ext_fn`.
* Non-template classes only (`Json::Value` yes, `Eigen::Matrix<...>` no: the plan is to instantiate a template with witness types, TODO.md). Typedefs and enums of the class and of its namespace are resolved.
* It is a prototype on clang's **text AST dump**, not the LibTooling tool of the plan (no LLVM development libraries on the machine it was written on). Fast enough (0.6 s for `Json::Value`), brittle against changes of the
  dump format; the tests pin the behaviour.
* Nothing was run against the library to confirm a fact (no sanitiser probes yet).

## Tested (CTest `cling_scan`)

`tests/fact-scan/`: a class with one method of every shape (the exact verdict per shape), the private library of `tests/facts/private_lib` (the scanned pack gives the same accept/reject results as the hand-written one),
the header-hash check, and **jsoncpp**'s `Json::Value` (46 facts, 7 methods left conservative): a CeeLing program using the pack builds against the real jsoncpp sources and runs, and a dangling use is rejected
(skipped without jsoncpp; set `JSONCPP_DIR`).
