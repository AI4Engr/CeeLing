# fact-gauge

Do the library facts and the checker agree, and do the facts agree with the C++ standard? (`PLAN_third_party_inference.md` section 7, "the gauge".)

```
python -I tools/fact-gauge/gauge.py --cling build/cling.exe [--report report.md]
```

Run by CTest as `cling_fact_gauge` (about 2.5 s). Three independent checks, any failure makes the exit code 1:

1. **Seeded dangling programs.** For every scanned `std::vector` and `std::map` method, a program that would dangle if the fact "this method changes the receiver" were ignored: an observer (`ref std::vector<int> r = v;` or `ref std::map<int, int> r = v;`, and
   `ref Node n = v[0];` over `std::vector<Node>`), then the call, then a use of the observer. The real compiler judges it. **False accept** (the fact says mutates, the program compiled) must be 0; **false reject**
   (the fact says no mutation, the observer died) must be 0. A method the compiler declines to admit is `REFUSED` (with its reason), a method whose every overload needs an argument CeeLing cannot write (an
   iterator) is `UNREACHABLE`; neither is a failure. A seed that fails for any other reason is `UNUSABLE` and fails the gauge: nothing is hidden.
2. **Usage corpus.** Every method the compiler admits, for `vector`, `string` and `map`, is called validly; cling must accept, and the generated C++ must compile with `g++ -std=c++17` (the standard the project builds
   with) and run to exit 0. This is the check that would have caught the C++20-only `starts_with` the first scan admitted.
3. **Facts against the standard.** `STANDARD_INVALIDATING` in `gauge.py` is a hand transcription of the standard's list of `std::vector` calls that may invalidate references, and of the `std::map` death events
   (`erase clear extract merge swap`; the standard is finer for a map, CeeLing kills observers on every mutator). A fact that says a listed method does not mutate is reported. This is the independent check: checks 1 and 2 only prove the compiler follows the facts; this is what found `reserve` missing from the hand table.

**Negative controls** (CTest, `WILL_FAIL`): `--pretend-mutator size` (the gauge believes `size` mutates, the compiler accepts the seed: must report false accepts) and `--pretend-nonmutator reserve` (a wrong fact:
must report it against the standard). A gauge that cannot fail proves nothing.

## Probing a SCANNED pack: `probe_pack.py` (the part that asks whether a fact is true of the library)

The three checks above are about `std` and about the compiler following the facts. A fact a scanner wrote (`tools/cling-scan`) can also be wrong about the LIBRARY, so `probe_pack.py` runs, per fact,
a CeeLing program through the compiler and the same probe as plain C++ against the real library under AddressSanitizer + UBSan (inside WSL: the MinGW toolchain has no AddressSanitizer). The recipe is a small
conf file (`tests/fact-scan/probe/eigen_vectorxd.conf`): how to build an object, a call that makes a view of it, how to read the view.

```
python -I tools/fact-gauge/probe_pack.py --cling build/cling.exe --pack vx.clfacts --conf tests/fact-scan/probe/eigen_vectorxd.conf -I C:/project/eigen
```

* `fresh`/`borrow` (the pack says the call leaves the receiver alone): the compiler must accept the program, and in the C++ probe the **lazy** view (`auto view = obj.head(2)`; CeeLing no longer lets you write that,
  and `T view = ...` would COPY) must stay readable after the call. A sanitiser report is a counterexample to the fact.
* `mutator` (changes the receiver): the compiler must reject; whether the library really invalidates is reported, not required (a mutator that invalidates nothing is merely conservative).
* **Result on `Eigen::VectorXd`** (CTest `cling_scan_probe`, 40 s): 56 facts, 0 contradictions: 54 `fresh`/`borrow` facts run clean, and the two mutators that really reallocate (`resize`, `conservativeResize`) are
  confirmed by `heap-use-after-free`. **Negative control:** declare `resize` and `conservativeResize` `fresh` and the probe fails with `heap-use-after-free` from the real library.
* **Second library, different shape: jsoncpp `Json::Value`** (a tree of deep-copied nodes; the library is compiled once under the sanitisers from its `.cpp` files, `sources =` in the conf): 41 facts, 0 contradictions
  (36 clean; `clear` really frees the node a reference points into, `heap-use-after-free`; 4 `UNREACHABLE`: their parameters are enums). Negative control: `clear` declared fresh is caught. The C++ twin holds a real
  reference into the object (`auto& view = obj["a"]`, the conf's `cxx_view_decl`/`cxx_view`), because `operator[]` is the way jsoncpp hands out views and the scanner does not write facts for operators.
* **Third library, a handle type: yaml-cpp `YAML::Node`** (shared memory behind copyable handles; the library's 25-odd `.cpp` files are built once under the sanitisers, `sources = @YAML@/src/*.cpp`): 13 facts, 0 contradictions, `SetTag`
  confirmed to free the buffer a raw pointer into the tag points at, and declaring it `fresh` is caught. `Node` has no view maker CeeLing can write (its views come from `operator[]`), so the conf leaves `view` empty and the report says the
  compiler half was skipped; the sanitiser half still runs. **Two argument sets per method:** a small one and one that forces allocation (a 300-character string, other numbers). With only the small one the `SetTag` control passed silently:
  assigning a shorter string reuses the old buffer, so nothing was freed. Same lesson as the lazy view: a probe that cannot fail is not a probe.
* **What the probe found about the scanner itself:** the first jsoncpp run failed to compile `obj.index()`: `Json::Value` had no such method. The scanner had read PAST the end of the class into the next node of clang's filtered
  dump (`Json::Value::CZString`, whose name also contains `Value`) and attributed its methods to `Json::Value`. The pack committed one step earlier contained those bogus facts. Fixed, with a regression test.
* Limits: one object, one or two views, one call per argument set, arguments chosen by a script; a clean run is evidence, not proof. It sees STORAGE going away or moving, not a value changing under the view (declaring `fill` fresh passes: `fill`
  rewrites values in place). A method that cannot be called with synthesised arguments is `UNREACHABLE`; a library assertion fired by the synthesised arguments is `NOT EVIDENCE`, never a pass.

### Added 2026-11-03: values rewritten in place, and a second view kind

* **State snapshot (`state =` in the conf).** The probe prints the object's observable state before the first call and after each call; a `fresh` or `borrow` method must leave it unchanged, otherwise `BEHAVIOUR CONTRADICTS FACT: ... observable state
  changed: 8 -> 20`. This is the check AddressSanitizer cannot do: a method that rewrites values in place frees nothing. The Eigen control: `--pretend-fresh fill` (previously invisible, and listed here as a limit) is now caught. Eigen: `state = obj.sum()`;
  jsoncpp: `toStyledString()`; yaml-cpp: `YAML::Dump(obj)`.
* **Second view (`cxx_view2_decl/cxx_view2/cxx_touch2`).** A raw pointer into the vector's storage (Eigen `data()`), an iterator into the tree (jsoncpp `begin()`), an iterator into the node (yaml-cpp). Both views are read after every call.
* **Recipe limits that stay.** One object per library: jsoncpp's recipe builds an object value, so array-only methods (`resize`, `append`) do nothing there, and the probe reports them as clean, which means "not exercised", not "verified". A conf with a
  second recipe (an array) would fix that; it has not been written. ThreadSanitizer is deliberately NOT used: no thread-safety claim is made anywhere, and a green race detector must not be read as one.

## What this does not show (read before quoting)

* Checks 1 and 3 use the facts and a transcription by the same author as their reference; check 3's list is not an independent audit of the standard.
* The seeds cover a local vector with a whole-vector observer and an element observer. An observer of a FIELD container is refused outright by the checker ("whose owner cannot be tracked"), which says nothing about
  the facts. `std::string` is seeded through a `string_view` since 2026-11-03 (an observer of its characters): all 41 names, every mutator kills the view, every reader leaves it.
* "0 false accepts" is a statement about these programs, one per method and context. It is not a proof, and no program shows that an inferred fact is true of the library's behaviour: that needs the sanitiser probes
  of phase 3 (TODO.md).
* Methods needing an iterator or initializer_list argument (`insert`, `erase`, `emplace`) cannot be written in CeeLing at all, so they are `UNREACHABLE`, not tested.
