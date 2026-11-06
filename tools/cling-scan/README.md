# cling-scan

Writes a draft fact pack for one C++ class from its declaration, so a project does not have to type the facts of its library by hand.

```
python -I tools/cling-scan/cling_scan.py --clang <clang++> --header grid.h --class Grid [-I include-dir ...] --out grid.clfacts       # a non-template class
python -I tools/cling-scan/cling_scan.py --clang <clang++> --header Eigen/Dense --type Eigen::VectorXd -I eigen --out vx.clfacts      # any type expression, templates too
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
| `function Cls::m` | `static`, and every parameter is a plain value (number, `bool`, enum, string). Whatever it returns, it has no object to refer to. (`Eigen::VectorXd::Ones(n)` returns an expression type; its only argument is a number.) |

plus `opaque Cls` first, so every method WITHOUT a fact keeps the conservative default (its result borrows the receiver, calling it kills the receiver's observers, an object argument is refused). When it cannot prove a
verdict it writes a `#` comment with the reason and no fact: a class returned by value ("the type alone does not say whether it refers to the receiver"), a non-const method returning a reference, overloads that disagree,
member templates, operators (write `operator` facts by hand). Every fact is `source=inferred review=unreviewed`; the pack header records the header's path and `sha256=` (`@header`), shown by `cling --report-facts`.

## What it does not do (read before trusting a pack)

* It reads **signatures**. `const` is taken at its word (a `mutable` member, a global or a pointer the class keeps can make a `const` method not fresh), and "returns a number" is the declared type. A scanner cannot see a method
  that returns an `int` which indexes the receiver's internals. The conservative direction is built in (anything it cannot prove stays opaque), but a **confidently wrong** inference is possible and is the same kind of hole as a wrong `ext_fn`.
* `--type` takes any type expression and instantiates it (`static_assert(sizeof(T) > 0)`), then reads the class **and its public base classes** (Eigen keeps most of its API in `DenseBase`, `MatrixBase`,
  `PlainObjectBase`...). Facts are named with the spelling you give. A template is scanned for the ONE instantiation you name: the facts of `VectorXd` are not claimed for `MatrixXf` (the plan's "witness types" check,
  scanning two instantiations and requiring equal facts, is not done here). Typedefs and enums of the classes and of the namespace are resolved. Member templates, operators and constructors are skipped.
* The compiler enforces that binding: `cling --facts pack` refuses a pack whose recorded headers changed (exit 2), warns about headers it cannot find, and `--strict-headers` makes a missing header an error.
* The pack binds **every non-system header the translation unit includes** (`clang -MM`; 265 for Eigen/Dense), not only the entry header, so a change anywhere in the library is noticed by `--check`.
* **LibTooling backend (2026-11-05):** `tools/cling-scan/libtooling/cling_scan_extract.cpp` is a C++ program on clang's AST (`ClangTool`, Sema's instantiated classes included), built by `libtooling/build.sh` against the LLVM/Clang development packages
  (Ubuntu: `clang-14 libclang-14-dev libclang-cpp14-dev llvm-14-dev`; one compile and link, about half a minute). It prints the methods as canonical type spellings and `cling_scan.py --backend libtooling` runs the same analysis on them. It reads
  **everything the text backend reads, templates (`--type`) and `--witness` included**, without parsing a dump: on `Shapes`, `Grid`, `Json::Value`, `YAML::Node` and `Eigen::VectorXd` / `Matrix3f` (69 facts each, 265 headers bound) it gives
  the same verdict for every method as the text backend (CTest `cling_scan_libtooling`). `auto` prefers it when the extractor is built (`--extractor`, `CLING_SCAN_EXTRACT`, or next to `build.sh`). On Windows it runs through WSL. Comparing the backends
  found a bug in the other two: a method whose name merely starts with `operator` (Eigen's `operatorNorm`) was skipped as an operator; it is a method and now gets its fact.
* **Three backends** (`--backend auto|libtooling|libclang|text`), one analysis. `libclang` (`libclang_backend.py`, since 2026-11-04) reads a non-template class through clang's **C API** with ctypes: a stable interface, so it does not break when the
  dump format changes, and it needs only the libclang shared library (no LLVM development headers; this is the stable C API, not LibTooling). `text` is the first version, which parses `clang -Xclang -ast-dump`: it needs `--clang`, is
  brittle against changes of the dump format (the tests pin it), and is the **only** way to read `--type`: the C API does not enumerate the members of a class template instantiation (tried on Eigen: `clang_visitChildren` on
  `Eigen::Matrix<double,-1,1>` yields only its template arguments, even after an explicit instantiation, and no base specifiers). `auto` picks libclang for `--class` when it finds the library, otherwise text; `--type` and `--witness` are text.
  The tests require both backends to give the same verdict for every method of `Shapes`, `Grid`, `Json::Value` (42 facts) and `YAML::Node` (13), with a negative control.
* libclang needs its own builtin headers (`stddef.h`, the intrinsics): the `libclang-common-NN-dev` package. Without it the backend falls back to gcc's include directory, which is enough for ordinary C++ and **not** for a library that
  includes `<immintrin.h>` (Eigen: gcc's header uses builtins clang lacks); `--clang-arg=-DEIGEN_DONT_VECTORIZE` avoids that include for Eigen. `--libclang <path>` (or `CLING_SCAN_LIBCLANG`) names the library; `--clang-arg` passes
  other arguments (`-resource-dir=DIR` works only with headers of the same clang version).
* Nothing was run against the library to confirm a fact (no sanitiser probes yet).

## Witness types: `--witness`

A template's facts depend on its arguments, so a pack for `Eigen::VectorXd` is not a pack for `Eigen::MatrixXd` (`cbegin`/`cend` differ). `--witness OtherType` (repeatable) scans the other instantiation too and **keeps a fact only where the verdict is
identical**; a method that differs gets no fact (it falls back to the conservative `opaque` default) and a `#` line saying which two verdicts disagreed. The pack records `@witness <type>: N fact(s) agree, M dropped`. On Eigen,
`VectorXd` with witnesses `MatrixXd` and `Matrix3f` keeps 66 facts and drops `cbegin`/`cend`. Limits: agreement among the instantiations you name is evidence about those, not about every instantiation; and the pack is still NAMED for one type, the
compiler does not know a pack was meant to be shared.

## Is a scanned fact true? `tools/fact-gauge/probe_pack.py`

The scan reads signatures; the probe checks the facts against the real library: the compiler judges one seeded program per fact, and a plain-C++ twin with a lazy view is built with AddressSanitizer and run. On
`Eigen::VectorXd` all 56 facts hold (54 clean, the 2 reallocating mutators confirmed), and declaring `resize` fresh is caught. See `tools/fact-gauge/README.md`.

Each overload's signature is written into the pack as a `# sig` comment (what the verdict was read from; `probe_pack.py` uses them to synthesise arguments). `fresh`, `borrow` and `mutator` additionally require every
PARAMETER to be a plain value (number, bool, enum, string; not an object, a pointer, or a non-const reference): "retains none of its arguments" only follows from the types then.

`const char *` parameters count as plain: in CeeLing the only source of one is a string literal (`c_str()` and `data()` are refused), which has static storage and cannot dangle. The same probe on **jsoncpp `Json::Value`** (a different
shape: a tree) holds for all 41 facts, and it caught a bug in this scanner (methods of a later class whose name contains the scanned one's leaked into the pack; fixed, regression-tested).

`std::size_t` and the other `std::`-qualified integer names count as numbers (found by yaml-cpp: `Node::size()` had no fact). `compare_packs.py a.clfacts b.clfacts` lists the methods whose verdict differs between two scans: on Eigen,
`VectorXd` and `MatrixXd` differ for `cbegin`/`cend` (borrow vs fresh) while `MatrixXd` and `Matrix3f` agree on all 68. A pack is for the type it was scanned from; scan the types you use, or compare first.

## Tested (CTest `cling_scan`)

`tests/fact-scan/`: a class with one method of every shape (the exact verdict per shape), the private library of `tests/facts/private_lib` (the scanned pack gives the same accept/reject results as the hand-written one),
the header-hash check, **jsoncpp**'s `Json::Value` (42 facts: a CeeLing program using the pack builds against the real jsoncpp sources and runs, and a dangling use is rejected), and **Eigen**'s `VectorXd`
(69 facts, 93 methods left conservative, 265 headers bound, scanned in 3 s: a CeeLing program using the pack builds against the real Eigen headers and runs, and a lazy `head()` view dies when `fill()` changes its owner).
The jsoncpp and Eigen parts are skipped when the library is absent (set `JSONCPP_DIR`, `EIGEN_DIR`).
