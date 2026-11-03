# std scan experiment: `std::vector<int>` and `std::basic_string<char>` (2026-11-01)

Question: can the facts the hand tables in `library_summary.cpp` encode (which members exist, what a result borrows, which calls invalidate)
be read off the standard library's own declarations, and how far do they agree? Nothing here touches the compiler.

Method (all in this directory; rerun with the commands at the end):

* `probe.cc` explicitly instantiates `std::vector<int>` and `std::basic_string<char>`, so clang's AST holds the members with `T` filled in.
* `clang++ -fsyntax-only -Xclang -ast-dump` on two library implementations: the MSVC STL (370,026 lines, 0.65 s) and libstdc++ 16.2 from the
  MinGW toolchain (231,922 lines, 0.49 s).
* `members.py` lists the direct public members of a specialization; `classify.py` derives, from **signatures only** (never bodies), the result
  shape (void/scalar, element reference, reference to self, iterator or view, raw pointer, fresh value, or UNRESOLVED), a mode for each parameter
  (read-only, consumed, mutated, template, raw pointer, iterator), and "mutates the receiver"; `diffimpl.py` compares the two libraries.
* Output: `vector.facts.tsv`, `string.facts.tsv` (MSVC STL) and `gcc/*.tsv` (libstdc++): one line per public overload.

## Results

| | `vector<int>` | `basic_string<char>` |
|---|---|---|
| public overloads / distinct names (MSVC STL) | 51 / 31 | 144 / 46 |
| public overloads / distinct names (libstdc++) | 50 / 30 | 148 / 46 |
| names whose facts are identical in both libraries | **30 of 30** (`get_allocator` only in the MSVC list, see 4) | 33 of 46 (the other 13: below) |
| overloads that return or take a raw pointer (sealed by rule) | 2 | 33 (MSVC), 35 (libstdc++) |
| template overloads needing a witness type | 4 | 24 |

1. **Signature-only facts agree across the two implementations** for `vector` (every name), and for `string` except 13 names
   (`append assign compare find find_first_not_of find_first_of find_last_not_of find_last_of insert operator+= operator= replace rfind`). Those
   13 differ only because libstdc++ spells its `string_view` overloads with a dependent return type (`_If_sv<_Tp, basic_string&>`) that the
   declaration text does not resolve; they come out UNRESOLVED (treated as "borrows self", conservative). A scanner needs the instantiation (a
   witness type) for them; the signature cannot say. The MSVC STL spells the same overloads with a plain return type, so there they resolve.
2. **"Mutates the receiver" can be derived from a structural rule.** Rule: a public non-`const` member that has no `const` twin (same name, same
   parameter list). It gives 13 vector names (`assign clear emplace emplace_back erase insert operator= pop_back push_back reserve resize
   shrink_to_fit swap`) and 14 string names (those plus `append operator+= replace`, minus `emplace`/`emplace_back`), and the 9 names with a
   `const` twin are exactly the accessors (`at back begin data end front operator[] rbegin rend`). Both lists equal what the standard lists as
   invalidating / non-invalidating, and the two libraries give the same sets. Caveats: the "standard" lists in `classify.py` are my transcription,
   not an independent audit; two classes is a small sample; and the rule says "may invalidate", not where or how much (the standard's
   "at or after the point of insertion" precision is not in any signature).
3. **Disagreements with the hand tables** (the useful output):
   * **`vector::reserve` is a mutator the standard says invalidates references (when capacity grows), but `vectorMethodInvalidatesReferences` leaves
     it out** (`library_summary.cpp`; the reasoning in `CLAUDE.md` is that no *observer of the vector* can see a reallocation, which is true, but
     an observer of an *element* can). Today this cannot be exploited: `reserve` is only admitted on primitive-element vectors, and `ref int x = v[0];`
     is accepted by `cling` but generates C++ that does not compile (see 5). It is safe by the accident of two other limits, not by a rule.
   * **The string has no invalidation table at all.** `append`, `clear`, `push_back` and `reserve` are admitted and the scan marks all four as
     mutators; the standard says they invalidate references, pointers and iterators into the string. Unobservable today because views of a string
     (`string_view`, character references) are not tracked (TODO.md, "not tracked"). It becomes a hole the day views are tracked unless the table is
     added with them.
   * The hand tables admit 7 of 31 vector names (22.6%) and 9 of 46 string names (19.6%); everything else they do not admit is "not yet" by policy.
     The scan finds no admitted name whose receiver-mutation the hand table gets wrong in the unsafe direction other than the two items above.
   * `copyWithin` is a CeeLing method with no `std` counterpart (expected, listed so nobody reads it as a gap).
   * The tables are keyed by method **name**. `string` has 144 overloads, of which 33 take or return a raw pointer; name-level admission of `find`
     or `append` admits them all. Harmless today (CeeLing has no raw-pointer values, a literal is the only source), but a fact keyed by name cannot
     say "this overload is sealed".
4. **Scanner failure modes found while doing it** (each produced a wrong answer in an earlier version of `classify.py`; they are the design
   requirements for a real scanner):
   * A result type written through a class typedef (`reference`, `const_reference`) was classified as a fresh value because `TypedefDecl` was not read
     (libstdc++ uses `typedef`, the MSVC STL uses `using`). **Wrong direction** (a borrow reported as fresh). Rule adopted: an unresolved type is
     never "fresh"; it is UNRESOLVED and treated as borrowing from `self`.
   * A raw-pointer test by substring `*` marked libstdc++ iterators (`__normal_iterator<int*, vector<int>>`) as raw pointers.
   * Members inherited from a base class are missed when only direct children are read (`get_allocator`, in libstdc++'s `_Vector_base`).
   * Dependent SFINAE return types (point 1) cannot be classified from text.
   A scanner built on clang's type API (canonical types, `getDesugaredType`, base-class walk, instantiation of witness types) removes the first
   three by construction; the fourth needs the witness instantiation either way. Text parsing of `-ast-dump` is enough for an experiment and
   should not be the product.
5. **Already-known bug, met again:** `int a = 1; ref int x = a;` is accepted by `cling` and emits `int* x = (a).get();`, which g++ rejects
   (`request for member 'get' ... of non-class type 'int'`). TODO.md lists it under "Accepted by CeeLing, rejected by g++ afterwards" (`ref` on a
   primitive or a vector). It matters here only because it is what keeps the `reserve` hole in item 3 closed.

## Addendum 2026-11-02: what happened next

* The shipped scans are redone at **C++17**, the standard the generated code is compiled with (a first scan at C++20 admitted `starts_with`/`ends_with`, which g++ -std=c++17 rejects). The numbers in the tables above are the C++20 scan.
* Unwrapping libstdc++'s `_If_sv<_Tp, X>` alias (its string_view-convertible overloads; X is the result when the overload is enabled) in `classify.py` resolves the 13 string names that came out UNRESOLVED: **all 46 string
  names and all 30 vector names are now identical across the two implementations.**
* `gen_pack.py` turns the two scans into `facts/std-vector.clfacts` and `std-string.clfacts`, and the compiler now computes which `vector`/`string` methods are admitted from them (no hand list). Result: `vector` admits
  11 callable names (was 8, of which `begin` and `end` are now refused), `string` 23 (was 9). The classic `auto it = v.begin(); v.push_back(2); *it` is rejected where it used to be accepted. Details: CLAUDE.md, TODO.md.
* The raw-pointer count in the table above (33/35 overloads "sealed by rule") overstated what is sealed: a `const char *` PARAMETER is how a string literal arrives and is harmless; only a raw-pointer RESULT (`data`,
  `c_str`) is sealed, plus a parameter that points to writable memory (`copy`, one `replace` overload), which keeps those methods out.

## What the scan does not give (signature level)

* **Retention** ("the callee keeps the argument"): `push_back(const int&)` copies, `push_back(Node&&)` moves; whether a *class-typed* argument is
  retained after the call needs the body or a witness analysis. The mode of the parameter (read-only / consumed / mutated) is derived; retention is not.
* **Where** an invalidation happens (the standard's per-member precision) and **how an iterator relates to which container** after `swap`.
* **Template overloads** (4 in vector, 24 in string): need witness instantiations.

## Decision input: how the effort splits

* The part that is mechanical and checked automatically: member lists, result shapes, parameter modes, raw-pointer sealing, "mutates receiver" via
  the twin rule, agreement between two libraries. For these two classes this is about 90% of the rows, with zero unsafe-direction disagreements once
  the UNRESOLVED rule is in.
* The part that needs a human with the standard open: one invalidation row per category (sequence, string, associative, unordered, ...), and the
  review of every UNRESOLVED and every disagreement between libraries. For these two classes that is a dozen rows plus 13 flagged string names (20
  overloads), not hundreds of facts.
* The part that stays hand-written for now: retention of class-typed arguments, which needs body analysis (phase 3 of `PLAN_third_party_inference.md`).
* Plain text per header group is adequate at this size: `vector.facts.tsv` is 3.7 KB, `string.facts.tsv` 16 KB (both libraries: 40 KB for the four files).

Limits of the experiment: two classes, two implementations of one language version, one author's transcription of the standard for the comparison
lists, signature level only, no run of the facts through the checker.

## Rerun

```
clang++ -std=c++20 -fsyntax-only -Xclang -ast-dump probe.cc > ast_msvc.txt            # MSVC STL (default on this machine)
clang++ -std=c++20 -fsyntax-only -target x86_64-w64-mingw32 -nostdinc++ \
    -isystem <mingw>/include/c++/16.2.0 -isystem <mingw>/include/c++/16.2.0/x86_64-w64-mingw32 -isystem <mingw>/include \
    -Xclang -ast-dump probe.cc > ast_gcc.txt
grep -n "ClassTemplateSpecializationDecl.*class \(vector\|basic_string\) definition.*explicit_instantiation" ast_*.txt   # the two line numbers
python -I classify.py ast_msvc.txt <vector-line> <string-line> .
python -I classify.py ast_gcc.txt <vector-line> <string-line> gcc
python -I diffimpl.py . gcc vector.facts.tsv
```
