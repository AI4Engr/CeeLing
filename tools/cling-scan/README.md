# cling-scan (prototype)

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
* The pack binds **every non-system header the translation unit includes** (`clang -MM`; 265 for Eigen/Dense), not only the entry header, so a change anywhere in the library is noticed by `--check`.
* It is a prototype on clang's **text AST dump**, not the LibTooling tool of the plan (no LLVM development libraries on the machine it was written on). Fast enough (0.6 s for `Json::Value`), brittle against changes of the
  dump format; the tests pin the behaviour.
* Nothing was run against the library to confirm a fact (no sanitiser probes yet).

## Is a scanned fact true? `tools/fact-gauge/probe_pack.py`

The scan reads signatures; the probe checks the facts against the real library: the compiler judges one seeded program per fact, and a plain-C++ twin with a lazy view is built with AddressSanitizer and run. On
`Eigen::VectorXd` all 56 facts hold (54 clean, the 2 reallocating mutators confirmed), and declaring `resize` fresh is caught. See `tools/fact-gauge/README.md`.

Each overload's signature is written into the pack as a `# sig` comment (what the verdict was read from; `probe_pack.py` uses them to synthesise arguments). `fresh`, `borrow` and `mutator` additionally require every
PARAMETER to be a plain value (number, bool, enum, string; not an object, a pointer, or a non-const reference): "retains none of its arguments" only follows from the types then.

`const char *` parameters count as plain: in CeeLing the only source of one is a string literal (`c_str()` and `data()` are refused), which has static storage and cannot dangle. The same probe on **jsoncpp `Json::Value`** (a different
shape: a tree) holds for all 41 facts, and it caught a bug in this scanner (methods of a later class whose name contains the scanned one's leaked into the pack; fixed, regression-tested).

## Tested (CTest `cling_scan`)

`tests/fact-scan/`: a class with one method of every shape (the exact verdict per shape), the private library of `tests/facts/private_lib` (the scanned pack gives the same accept/reject results as the hand-written one),
the header-hash check, **jsoncpp**'s `Json::Value` (42 facts: a CeeLing program using the pack builds against the real jsoncpp sources and runs, and a dangling use is rejected), and **Eigen**'s `VectorXd`
(69 facts, 93 methods left conservative, 265 headers bound, scanned in 3 s: a CeeLing program using the pack builds against the real Eigen headers and runs, and a lazy `head()` view dies when `fill()` changes its owner).
The jsoncpp and Eigen parts are skipped when the library is absent (set `JSONCPP_DIR`, `EIGEN_DIR`).
