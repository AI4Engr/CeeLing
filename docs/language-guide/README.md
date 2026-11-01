# The CeeLing Language Guide

CeeLing is a small, Java-looking language that compiles to ordinary C++ and refuses to produce a program in which a reference can outlive its object. You write objects and method calls;
the compiler picks the smart pointers, inserts the moves, rejects dangling references and use after move, and adds run-time checks for indexing and null. The output is plain C++ that
works with the standard library, Eigen and any other C++ code.

This guide is a book. Read chapter 1 first: it shows, with real programs, what goes wrong in C++, why the C++ compiler cannot see it, and what CeeLing does instead. The other chapters
take each feature in turn. In every chapter a feature is explained the same way: the C++ problem (with a C++ program that compiles and is wrong), why C++ does not stop it, what CeeLing
does and why it can, and where it stops.

Every program in the guide is a file in [`examples/`](examples/README.md), compiled and run by a checker; the C++ twins sit beside them. Run any of them with `examples/run.sh NAME`.

## Contents

**Start here**

1. [Why CeeLing](01-why-ceeling.md): the promise, nine bugs C++ accepts, what you write, what it costs.
2. [The language](02-language.md): classes, structs, inheritance, enums, namespaces, operators, errors, and what is not there.
3. [Values, text and containers](03-values-and-containers.md): numbers, strings, vectors, maps, the library allow-list.

**The guarantees**

4. [Ownership](04-ownership.md): the compiler chooses `unique_ptr`, `shared_ptr` or `weak_ptr`.
5. [Moves](05-moves.md): hand-over by default, `std::move` at the last use, use after move.
6. [Dangling references](06-dangling-references.md): `ref` observers, death events, containers, views.
7. [Run-time checks](07-run-time-checks.md): bounds and null.
8. [Sealed escape hatches](08-sealed-escape-hatches.md): what the language removes and why.
9. [Closures](09-closures.md): capture by value only.
10. [Threads](10-threads.md): `spawn` and `Channel`, and exactly what they promise.

**Working with the C++ world**

11. [Talking to C++](11-talking-to-cpp.md): `ext_fn`, the trust boundary, the generated C++ as a library.
12. [Programs of many files](12-many-files.md): `#include "x.clh"`, separate compilation, JSON decoders.

**Evidence and context**

13. [The contract and the threat model](13-contract-and-threats.md): what is promised, row by row, and what is open.
14. [How the analysis works](14-how-it-works.md): summaries, provenance, death events.
15. [Case study: cJSON](15-case-study-cjson.md): all 33 published vulnerabilities, in C and in CeeLing.
16. [Case study: app-cling](16-case-study-app-cling.md): a real program, and the pointers the compiler inferred for it.
17. [CeeLing, Rust, Circle and profiles](17-comparisons.md): an honest comparison.
18. [Status and roadmap](18-status.md).

## Keeping the guide true

`python3 -I docs/language-guide/examples/check.py check` runs every example, compares every code block, compiler message and program output in the chapters with what the compiler really
does, and fails if an example is not shown by any chapter. It is part of the test suite (`cling_guide_examples`). How to write a chapter: [CONTRIBUTING.md](CONTRIBUTING.md). The
development history of the compiler, which the guide leaves out, is in [`../internals/`](../internals/).
