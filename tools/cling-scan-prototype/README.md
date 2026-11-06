# cling-scan prototype (2026-10-18) -- exploratory, NOT a product

Measures how much of a C++ library's borrow information clang can extract
automatically. Run against `Eigen::MatrixXd` and its five base classes.

Pipeline (what actually worked):
1. `clang++ -fsyntax-only -Xclang -ast-dump=json probe.cc > full.json`
   (emsdk clang 24; ~1 GB for Eigen, ~2.5 s). Do NOT use `-ast-dump-filter=<class>`:
   it returns scattered slices and silently misses members pulled in through
   `.inc` plugin files (`block`, `row`, `resize`, `data`... were all absent).
2. `enum4.py` -- stream the JSON with `ijson` (pip, C backend, ~5 s for 1 GB),
   walk `namespace Eigen` -> `ClassTemplateDecl` -> `ClassTemplateSpecializationDecl`,
   keep only specializations whose receiver is `Matrix<double,-1,-1>`, and write
   every member (name, parameter types, const, static, member-template) to JSON.
3. `gen5.py` -- synthesize arguments from parameter types and emit one
   `Show<decltype(CALL)> pN;` line per member. `decltype` is an unevaluated operand,
   so argument VALUES are irrelevant, only their types.
4. `clang++ -fsyntax-only -fno-caret-diagnostics -fmessage-length=0 -ferror-limit=0`
   -- each `implicit instantiation of undefined template 'Show<T>'` error carries the
   REAL, alias-expanded return type `T`.
5. `classify5.py` -- bucket T (primitive / reference / pointer / owner / class holding
   the receiver type).
Dead ends (do not repeat): libclang 18 from pip cannot parse GCC 16's libstdc++ headers
and returns an empty specialization; reading return types straight from the JSON gives
unexpanded aliases (`CwiseCbrtReturnType`, `Scalar`) and ~58% "unclassified";
explicit instantiation (`template class Matrix<double,-1,-1>`) trips Eigen's own
static asserts.
Results and caveats are recorded in the internal notes ("cling-scan feasibility measurement").
