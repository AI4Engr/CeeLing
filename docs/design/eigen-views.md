# Design record: tracking Eigen views

**Status: implemented (v1).** A call to one of eight Eigen methods returns a view of its receiver, and the checker treats the result as an observer of that receiver. This page keeps the reasoning for the design, which is still the reason Eigen is handled the way it is. It is not a description of the code; for behaviour read [guide chapter 6](../language-guide/06-dangling-references.md#eigen) and [chapter 11](../language-guide/11-talking-to-cpp.md). How the same idea grew into fact packs for any library is in [third-party-inference.md](third-party-inference.md).

## The problem

A method such as `a.block(0, 0, 2, 2)` on an `Eigen::MatrixXd` does not copy anything. It returns an expression object that refers to `a`'s storage. If `a` is then reassigned or moved away, the result refers to freed memory:

```
Eigen::MatrixXd a = Eigen::MatrixXd::Identity(3, 3);
Eigen::MatrixXd b = a.block(0, 0, 2, 2);    // b refers to a's storage
a = Eigen::MatrixXd::Identity(5, 5);        // a's storage is replaced
std::cout << b << std::endl;                // use of dangling observer `b` (its owner `a` is gone)
```

Before this work such a call was rejected outright ("call to unknown function/method `block`"), the safe default for a library the compiler knows nothing about. The aim was to accept the common view methods *and* catch the dangling use.

## Why Eigen is a different shape from CeeLing's own classes

1. **Eigen is never parsed.** `Eigen::MatrixXd` arrives through a system `#include` that the parser does not read, so no class declaration exists to derive a borrow summary from. The mechanism that gives CeeLing's own methods their summaries (it walks a class's members) has nothing to walk. Eigen needed a parallel route, not a way of faking a declaration.
2. **Its declarations cannot reasonably be scanned.** A method such as `block` is declared with macros, a dependent return type, in an `.inc` file included into the middle of a class body, four or five template layers into a CRTP hierarchy, and `MatrixXd` is only a `typedef` of `Matrix<double,-1,-1,0,-1,-1>`. At the time of this work the conclusion was not to write a signature parser. (Later, `tools/cling-scan` did read Eigen's declarations by asking a real compiler's AST instead; see below.)
3. **The borrow is legible from the return type's name.** `col()` returns `ColXpr`, an alias of `Block<...>`, whose constructor takes the source expression by reference and stores it. That pattern holds across Eigen's expression templates: `Block`, `Transpose`, `Ref`, `Map`, `Diagonal`, `Reverse`, `CwiseBinaryOp`, `CwiseUnaryOp`, `Product`, `Replicate` and `Select` refer to their operands, while `Matrix`, `Array` and `PlainObjectBase` own storage. So the rule is a classification by type name, not a parse of the signature. (Read from the Eigen sources in 2026-10; the exact files and lines are not reproduced here because they change between Eigen versions.)
4. **CeeLing has no `auto`, which is what makes this enough.** In C++ the classic Eigen trap is `auto e = A + B;`, which holds a lazy expression that refers to `A` and `B`. CeeLing requires a declared type, and `Eigen::MatrixXd e = A + B;` forces evaluation into an owned matrix. The expression-template case is closed by construction, so only the narrower case remains: a named result of a view method. See [why `auto` is not supported](../language-guide/02-language.md#what-is-not-in-the-language-and-what-to-write-instead).
5. **No run-time or code-generation cost.** The classification feeds the checker's existing table of observers. The emitter produces an ordinary method call, and Eigen keeps its lazy, zero-copy behaviour in the generated C++.

## What was decided for v1

* **In scope:** `receiver.method(args)` where the result borrows the receiver: `block`, `row`, `col`, `transpose`, `segment`, `head`, `tail`, `diagonal`, on `Eigen::MatrixXd`. The list is a set of facts (`borrow Eigen::MatrixXd::block ...` in `facts/eigen.clfacts`), hand-written and not exhaustive. A method not on the list is rejected, which is the safe failure.
* **Out of scope:** expressions that borrow several operands (`a + b`; moot without `auto`); a general file format for other libraries (this became the fact packs); chained views (`a.block(...).transpose()`), which are rejected rather than composed.
* **Not a goal:** Eigen's storage semantics (row or column major, overlapping blocks). The question answered is only "does this result observe the receiver".

## The one hard design point: receiver, not argument

The observer machinery already existed for a function result that borrows one of its *arguments*, and an argument handed to such a function is consumed by the move rules. A view method borrows its *receiver*, and `a.block(...)` must **not** consume `a`: `a` stays usable, and only a later reassignment, move or end of scope of `a` kills the view. The implementation therefore has its own entry point for receiver borrows (`DanglingChecker::resolveBorrowingMethodReceiver`, called where the declaration and assignment forms are analysed) that registers the observer without marking the receiver moved, and then reuses the same death events and use-site check.

## What exists now, and how it is checked

* Facts: `facts/eigen.clfacts`. The checker: `resolveBorrowingMethodReceiver` in `src/passes/dangling_checker.cpp`.
* Rejected: reassigning the source, passing the source to a function (a move), then using the view: `tests/safety/eigen_block_dangles_after_reassign_rejected.clg` and `eigen_block_dangles_after_move_rejected.clg`, and the checked guide example `06_eigen_block_dangles.clg`.
* Probed by hand on 2026-11-09 with the current compiler, and **not** fixtures: using the view while the source is alive, taking a second view (`a.row(0)`) after the first, and copying the source into another matrix are all accepted; a chained view and an unlisted method (`a.inverse()`) are rejected. Accepted fixtures would need Eigen's headers to compile and run, and the machine this was written on has none.
* Beyond the eight hand-written facts: `tools/cling-scan` can draft a pack from Eigen's real declarations (it produced 69 facts for `VectorXd`, bound to the headers by hash), and the facts were checked against the real library under AddressSanitizer. That work is described in [third-party-inference.md](third-party-inference.md).

## What this does not cover

Only the listed methods on `Eigen::MatrixXd` are views the checker knows. Another Eigen type needs its own facts. Whether a fact is true of the library is the pack author's claim, not something the compiler verifies.
