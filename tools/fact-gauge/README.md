# fact-gauge

Do the library facts and the checker agree, and do the facts agree with the C++ standard? (`PLAN_third_party_inference.md` section 7, "the gauge".)

```
python -I tools/fact-gauge/gauge.py --cling build/cling.exe [--report report.md]
```

Run by CTest as `cling_fact_gauge` (about 2.5 s). Three independent checks, any failure makes the exit code 1:

1. **Seeded dangling programs.** For every scanned `std::vector` method, a program that would dangle if the fact "this method changes the receiver" were ignored: an observer (`ref std::vector<int> r = v;`, and
   `ref Node n = v[0];` over `std::vector<Node>`), then the call, then a use of the observer. The real compiler judges it. **False accept** (the fact says mutates, the program compiled) must be 0; **false reject**
   (the fact says no mutation, the observer died) must be 0. A method the compiler declines to admit is `REFUSED` (with its reason), a method whose every overload needs an argument CeeLing cannot write (an
   iterator) is `UNREACHABLE`; neither is a failure. A seed that fails for any other reason is `UNUSABLE` and fails the gauge: nothing is hidden.
2. **Usage corpus.** Every method the compiler admits, for `vector` and `string`, is called validly; cling must accept, and the generated C++ must compile with `g++ -std=c++17` (the standard the project builds
   with) and run to exit 0. This is the check that would have caught the C++20-only `starts_with` the first scan admitted.
3. **Facts against the standard.** `STANDARD_INVALIDATING` in `gauge.py` is a hand transcription of the standard's list of `std::vector` calls that may invalidate references. A fact that says a listed method
   does not mutate is reported. This is the independent check: checks 1 and 2 only prove the compiler follows the facts; this is what found `reserve` missing from the hand table.

**Negative controls** (CTest, `WILL_FAIL`): `--pretend-mutator size` (the gauge believes `size` mutates, the compiler accepts the seed: must report false accepts) and `--pretend-nonmutator reserve` (a wrong fact:
must report it against the standard). A gauge that cannot fail proves nothing.

## What this does not show (read before quoting)

* Checks 1 and 3 use the facts and a transcription by the same author as their reference; check 3's list is not an independent audit of the standard.
* The seeds cover a local vector with a whole-vector observer and an element observer. An observer of a FIELD container is refused outright by the checker ("whose owner cannot be tracked"), which says nothing about
  the facts. `std::string` has no seeds: no observer of its contents can be written yet, so its `mutates` facts are untested.
* "0 false accepts" is a statement about these programs, one per method and context. It is not a proof, and no program shows that an inferred fact is true of the library's behaviour: that needs the sanitiser probes
  of phase 3 (TODO.md).
* Methods needing an iterator or initializer_list argument (`insert`, `erase`, `emplace`) cannot be written in CeeLing at all, so they are `UNREACHABLE`, not tested.
