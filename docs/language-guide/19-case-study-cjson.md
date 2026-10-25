# 19. Case Study — cJSON's 33 Published Vulnerabilities

Chapters 10–17 argue that CeeLing prevents classes of memory bugs. This chapter tests that
argument on a real library with a real, independently published bug list: **cJSON 1.7.19**, a
widely vendored C JSON parser, and the 33 vulnerabilities Joshua Rogers published against it in
July 2026 ([*33 Vulnerabilities in cJSON*](https://joshua.hu/cjson-json-parser-cve-vulnerabilities)).

The method: port the whole library to CeeLing (`cjson-cling`), prove the port behaves like the
original, then take each of the 33 issues and ask what happens to it in the port.

## The short answer

| Outcome in the CeeLing port | Issues | Count |
|---|---|---|
| **Cannot happen** — the unsafe operation cannot be written | 1, 2, 3, 4, 7, 12, 13, 30, 32 | 9 |
| **Stopped at run time** — aborts with the source line instead of corrupting memory | 5, 11 | 2 |
| **Partly** — one route closed, another still open | 6, 8, 9 | 3 |
| **Not prevented** — CPU exhaustion | 10 | 1 |
| **Avoided by the library's own discipline, not by the language** | 31 | 1 |
| **Same bug, same behaviour** — a logic error; memory-safe in C too | 14–29, 33 | 17 |
| | | **33** |

Of the 13 memory-safety and crash issues (1–13), **9 are cut off** (7 cannot happen, 2 are
caught at run time), 3 are partly open, and 1 is not prevented. Of the 20 later issues, **17 are
logic errors that CeeLing does not and cannot prevent** — they are faithfully reproduced — and the
other 3 are undefined-behaviour and global-state bugs (30, 31, 32), two of which the language removes. This is
the honest shape of the result: CeeLing removes the bugs that come from *manual memory
management*; it does nothing for bugs in *what the program means*.

## 19.1 What was built, and how we know it behaves like the original

`cjson-cling` is a CeeLing port of `cJSON.c` and `cJSON_Utils.c` (JSON Pointer, Patch, Merge
Patch, sorting): about 2,900 lines of `.clg`, plus a 30-line hand-written C++ shim for number
formatting (`printf("%g")` and `strtod`) — the library's only trusted code. The real data structure
is kept: nodes with `next` / `prev` / `child`, where the first child's `prev` points at the last
child.

Equivalence is *measured*, not assumed:

- **Differential test, core** — the port and upstream parse the same bytes (upstream's inputs,
  115 hand-picked corner cases, and 1.5 million mutations) in four modes; success, error
  position, end offset and every printed byte must match. 0 differences.
- **Differential test, Utils** — pointer lookup, `FindPointerFromObjectTo`, sorting, patch
  generation and application (status code *and* resulting document), merge patches, against the
  RFC 6902 suites shipped with upstream, plus targeted escape cases: ~260,000 comparisons,
  0 differences.
- **Upstream's own unit tests**, ported: 776 checks pass.
- **Under sanitizers** (Ubuntu 22.04 in WSL, g++ 11.4, `-fsanitize=address,undefined
  -fno-sanitize-recover=all`, built *without* the pooled allocator so a use-after-free is visible): the 776
  unit checks, 756,504 differential parses and 350,298 Utils comparisons produce **no ASan, UBSan or
  LeakSanitizer report**. Upstream's own C is not sanitized in this run; the claim under test is about the
  port.
- The harness is itself tested: four deliberately seeded bugs (a depth limit off by one, 14 digits
  instead of 15, a wrong status code, the `~1` decode index) were each caught.

> **A caveat that matters.** The 33 triggers in this chapter are *reconstructions*. I worked from
> the article's one-sentence description of each defect and wrote the smallest program in the
> port that does the same thing; I did not use the author's proofs of concept. A reconstruction can
> miss the exact path the original exploit takes. Each "cannot happen" below therefore rests on a
> *mechanism* (what the language no longer lets you write), not only on a passing test.

## 19.2 Why cJSON has these bugs

The 33 issues look like a long list, but they come from five causes.

**1. Lifetime is managed by hand, with flags.** A cJSON node owns its children and strings *unless*
a flag says otherwise (`cJSON_IsReference`, `cJSON_StringIsConst`). Every function that frees or
replaces something must remember who else might still point at it. That is an ownership system
built from convention, and each function re-implements it. Issues 1–4 and 7 are places where the
convention was applied wrongly (or not at all) — a use-after-free is what a wrong guess costs.

**2. A pointer can be `NULL`, and C does not say so.** Every parameter is a `cJSON *`; whether
`NULL` is allowed depends on the function and on its callers. Some paths dereference without
checking (5, 11, 12) and some write to the result of `malloc` without checking it (13).

**3. Recursive algorithms on a structure that comes from untrusted input.** Parsing, printing,
comparing, duplicating and deleting all recurse once per nesting level. Nothing bounds the depth of
a tree built through the API, or by a patch that copies a subtree into itself (6, 8, 9), and one
function recurses twice per level (10).

**4. The code does what it says, and what it says is wrong.** Most of the later issues (14–29, 33)
are not memory errors at all: a pointer decoder that writes to the wrong index, a minifier that
ignores backslash parity, a comparison that treats duplicate keys oddly. They are specification
mistakes, and they are in cJSON because cJSON is software.

**5. Undefined behaviour and shared global state.** Comparing pointers from different allocations
(30), casting `NaN` to `int` (31), and a `static` error record shared by every caller (32).

CeeLing addresses causes 1, 2 and 5 directly, helps with cause 3 only in one place, and is
silent on cause 4. The rest of the chapter shows each of those claims against the evidence.

## 19.3 Lifetime bugs (issues 1, 2, 3, 4, 7)

### The problem in C

| # | Where | What goes wrong |
|---|---|---|
| 1 | `overwrite_item` | frees a node's child, key and string without honouring the reference/const flags, so it frees memory the node does not own |
| 2 | `apply_patch` / `ApplyPatches` | reads the next patch after an earlier patch may have deleted it, when the target tree contains the patch tree |
| 3 | `merge_patch` | keeps walking and duplicating nodes it has already deleted when target and patch alias each other |
| 4 | `ReplaceItemViaPointer`, `ReplaceItemInObject` | frees the replaced item while the library still points at it; no guard against a node replacing its own parent |
| 7 | `ParseWithOpts`, `GetErrorPtr` | returns early on `NULL` without resetting the saved error state, so the error pointer can reference a buffer the caller already freed |

All five share one root: *someone calls `free` while someone else still holds a pointer*.

### How CeeLing solves it

There is no `free`. CeeLing has no `delete`, no `malloc`, no raw pointer to dangle and no
`.get()` to extract one (Chapter 17), so the sentence "free this while that still points at it"
cannot be written. Ownership is inferred (Chapter 10): a node reachable from two places is a
`shared_ptr`, and it lives as long as the last reference does. The `isReference` flag survives
only as data — it records "do not edit this string", and nothing depends on it for correctness.
`prev` is a weak link (`//@ptr weak Item.prev`), so the back-pointer cannot keep a node alive and
cannot outlive it. Issue 7 disappears for a different reason: the error position is now a *value*
in the result (`ParseResult.errorPos`), not a pointer into the caller's buffer plus hidden global
state.

### Proof by example

Each is a program in `cjson-cling/tests/vulns/`, run by `tests/run_vulns.sh`, which checks the
outcome the file declares.

```
// 02_patch_deletes_itself.clg  — the patch removes the array that holds it
Item doc = parse("{\"p\":[{\"op\":\"remove\",\"path\":\"/p\"},{\"op\":\"add\",\"path\":\"/q\",\"value\":1}]}");
Item patches = getObjectItem(doc, "p");
int status = applyPatches(doc, patches);
//  →  status 0 doc {"q":1}      both operations applied; nothing was read after being freed
```

```
// 01_overwrite_reference.clg  — a root-level replace on a reference node
Item original = parse("[1,2,3]");
Item ref = createArrayReference(original.child);
applyPatches(ref, parse("[{\"op\":\"replace\",\"path\":\"\",\"value\":9}]"));
//  →  reference 9  original [1,2,3]     the original is untouched
```

| # | Test | Result |
|---|---|---|
| 1 | `01_overwrite_reference` | original intact after the replace |
| 2 | `02_patch_deletes_itself` | both patches applied, no stale read |
| 3 | `03_merge_patch_alias` | defined result; no walk over freed nodes |
| 4 | `04_replace_with_ancestor` | `replace returned 0`, tree unchanged (see 19.5 for why it returns 0) |
| 7 | `07_stale_error_pointer` | a failed parse reports position 3; a later empty parse reports 0 — no pointer exists to go stale |

**What CeeLing did and did not do here.** Honestly, the work in this class is done by the
*absence of manual memory management* plus ordinary C++ smart pointers underneath. The dangling-
reference analysis of Chapter 11 played no part: the library never uses a `ref` observer. The
credit belongs to Chapter 17 (the sealed hatches) and Chapter 10 (inference), and to the fact that
the port had to be written *without* the tools that make these bugs possible.

## 19.4 Null bugs (issues 5, 11, 12, 13)

### The problem in C

| # | Where | What goes wrong |
|---|---|---|
| 5 | `DetachItemViaPointer` | writes through `parent->child` without checking it is non-`NULL` |
| 11 | `apply_patch` | dereferences the target's key without a check when a root-level add/replace is applied to a `NULL` object |
| 12 | `apply_patch`, `detach_path` | reads a string node's value after checking only its *type*, so a string node whose value is `NULL` crashes them |
| 13 | allocation sites in `cJSON_Utils.c` | write to the result of `cJSON_malloc` without checking for `NULL` |

### How CeeLing solves it

Two different mechanisms, because the four bugs are two different kinds of null.

**A null object handle (5, 11): a run-time check on every dereference.** This is the "runtime
backstop" of Chapter 14, and it did not exist until this case study needed it. Before, only a
`weak_ptr` was checked; an empty `shared_ptr` was an ordinary C++ null dereference. Now every `->`
the generator emits for a member access, a method call or an indexing goes through
`cling::nn(handle, "line N")`, which aborts with the **source line** instead of crashing or
corrupting memory. It is on by default (`--no-null-checks` turns it off); it costs nothing
measurable on the cJSON benchmark (parse 50.0 MB/s with the checks and 50.0 without).

**A null string (12) and a null allocation (13): the thing does not exist.** A CeeLing `string` is a
value and is never `NULL`; there is no `malloc` whose result could be. An allocation failure is a
`std::bad_alloc` that ends the program, not a silent write through `NULL`.

### Proof by example

```
// 05_detach_null_child.clg  — the claimed parent has no child, but the item's prev is set
Item parent = createArray();
Item item   = createNumber(2.0);
item.prev   = createNumber(1.0);
detachItemViaPointer(parent, item);
//  →  stderr: "cling: null dereference at line 447"      exit code 3
```

```
// 11_patch_null_root.clg  — a root-level "add" applied to a NULL object
Item nothing = nullptr;
applyPatches(nothing, parse("[{\"op\":\"add\",\"path\":\"\",\"value\":1}]"));
//  →  stderr: "cling: null dereference at line 608"      exit code 3
```

The port is *faithful* here on purpose: it does not add its own `if (object == nullptr)` guard (an
earlier draft did, and it hid the bug). The language's check is what is being demonstrated.

| # | Test | Result |
|---|---|---|
| 5 | `05_detach_null_child` | **caught at run time**, abort at the dereference's line |
| 11 | `11_patch_null_root` | **caught at run time**, abort at the dereference's line |
| 12 | `12_string_without_value` | no crash: the missing value reads as an empty string (status 0, a path of `""`) |
| 13 | `13_alloc_unchecked` | no `malloc` in the port; the named function works and returns `/a/1` |

**Be careful what "caught" means.** The program still stops. A run-time check turns *silent memory
corruption* into a *clean, located abort*; it does not make the bug's input work. For 12 the port
does something arguably worse than a crash — it quietly treats a malformed node as an empty path —
because CeeLing's strings cannot be null and the library had to say *something* for the "no value"
state. That is a design choice the port made, recorded in `GAPS.md`.

## 19.5 Recursion and cost (issues 6, 8, 9, 10)

### The problem in C

| # | Where | What goes wrong |
|---|---|---|
| 6 | `ReplaceItemInArray` | lets an array be put inside itself and reports success; `Delete` then recurses forever |
| 8 | `Duplicate` | its depth cap (10,000) is high enough that the stack can run out before the cap triggers |
| 9 | `Delete`, JSON Patch `copy` | `Delete` has no depth bound; a `copy` can build trees deep enough to overflow the stack |
| 10 | `Compare` | recurses twice at every object level, so run time doubles per nesting level |

### How CeeLing solves it — and where it stops

This is the class where the answer is *partly*, and the "partly" is the useful part.

**Freeing a deep tree (9, the `Delete` half): solved by the compiler.** A class that owns another
object of its own type (`next`, `child`) gets a generated **iterative destructor**: it drains the
links onto a heap work list and frees them in a loop, so the depth is 1. A node that something else
still holds is released, not dismantled. This was found by probe, not by thought: a 1,000,000-node
chain printed its result and then crashed on scope exit (100,000 nodes already crashed; 10,000 did
not), because `shared_ptr` destruction recurses once per node. No CeeLing program has to defend
against this any more.

**Putting a node inside itself (6, 4): one line of library code, not the language.** CeeLing does
not detect ownership cycles, and nodes do not know their parent. The port adds a single check —
`replaceItemViaPointer` refuses when the replacement *is* the parent — and that closes the direct
form of 4 and 6. It does *not* close a cycle one step further away.

**Everything else (8, 9's print half, 6's indirect form, 10): not addressed.** Printing, comparing
and duplicating still recurse once per level, and CeeLing has no stack-depth check.

### Proof by example

```
// 06_array_into_itself.clg                      // 06b_cycle_through_two_arrays.clg
Item a = parse("[1,2,3]");                       Item a = createArray();  Item b = createArray();
replaceItemInArray(a, 0, a);                     addItemToArray(a, b);    addItemToArray(b, a);
//  → replace returned 0 (refused)               printUnformatted(a);
                                                 //  → the process dies: stack exhausted (exit 127)
```

| # | Test | Result |
|---|---|---|
| 6 | `06_array_into_itself` | **refused** by the library check |
| 6 | `06b_cycle_through_two_arrays` | **still open**: printing the two-array cycle exhausts the stack |
| 8 | `08_duplicate_deep` / `08b_duplicate_limit` | 9,990 levels duplicate fine on this machine's default stack; 10,001 levels are refused at the same limit as upstream. Stack-size dependent, no language check |
| 9 | `09_delete_deep` | **fixed**: a 300,000-level tree is released without trouble |
| 9 | `09b_print_deep` | **still open**: printing the same 300,000-level tree exhausts the stack |
| 10 | `10_compare_exponential` | **not prevented**: comparing two 24-deep object chains takes 0.79 s, doubling per level |

**Why this matters beyond cJSON.** Items 6 and 9 say the same thing from two sides: CeeLing makes the
*memory* side of a cycle or a deep tree safe, and leaves the *resource* side alone. A
`shared_ptr` cycle is a leak, not a corruption; a stack overflow is a crash, not a corruption. Both
are outside the safety contract (Chapter 2) — and a real service would still be taken down by the
second one.

## 19.6 Undefined behaviour and shared state (issues 30, 31, 32)

### The problem in C

| # | Where | What goes wrong |
|---|---|---|
| 30 | `SetValuestring` | compares pointers from unrelated allocations (undefined behaviour) and wrongly refuses legitimate calls such as assigning an item's own string back to itself |
| 31 | `CreateNumber`, `SetNumberHelper` | cast a `NaN` double to `int` (undefined behaviour) |
| 32 | `Version`, the parse path's `global_error` | `static` shared state that concurrent callers race on |

### How CeeLing solves it

- **30** — there are no pointers to compare. A string is a value; the "does the new text overlap the old
  buffer?" question has no meaning, so the legitimate self-assignment simply works.
- **31** — CeeLing's `int(x)` conversion is `static_cast`, which is *still* undefined for `NaN` and for
  out-of-range values. The port avoids it the same way a careful C author would: `saturateToInt`
  tests for `NaN` and the int range first. This one is the library's discipline, not the language's.
- **32** — the compiler rejects **file-scope variables** (`file_scope_variable_rejected.clg`; before
  that check they were silently dropped, which is a bug the case study also found). The port has no
  `static` record: the error position is returned in the result and `version()` returns a literal.

### Proof by example

```
// 30_set_valuestring_self.clg                     // 31_nan_to_int.clg
Item s = createString("hello world");              Item n   = createNumber(0.0 / 0.0);
setValuestring(s, s.valuestring);                  Item big = createNumber(1.0e30);
//  → true (upstream refuses)                      //  → nan->0   1e30->2147483647
```

**What this does not show.** Issue 32 is "cannot happen" *for this library's own state*. CeeLing is
single-threaded by contract (Chapter 4, threat 19): two threads sharing one tree can still race on
its fields, and nothing checks that. What the case study checks instead is that two threads each
building **their own** trees through the (pooled) allocator do not corrupt each other —
`tests/run_threads.sh`, 8 threads, 0 errors, and a deliberately broken variant that segfaults.

## 19.7 Logic bugs (issues 14–29 and 33)

### The problem in C

These are the bulk of the list, and none is a memory error:

| # | Where | The defect |
|---|---|---|
| 14 | `sort_list`, `sort_object` | leave the new head's tail pointer unset, so a later append is silently dropped (and leaked) |
| 15 | `decode_pointer_inplace` | decodes `~1` into the wrong position and never copies ordinary characters after an escape |
| 16 | `decode_pointer_inplace` | stops quietly on an invalid `~` escape instead of failing |
| 17 | `decode_array_index_from_pointer` | accumulates digits with no overflow check, so a huge index wraps to a small valid one |
| 18 | `decode_array_index_from_pointer` | accepts an empty token as index 0 |
| 19 | `get_item_from_pointer` | resolves any non-empty pointer that does not start with `/` to the root |
| 20 | `detach_path` | uses the case-sensitive flag for the parent but a case-insensitive lookup for the child |
| 21 | `generate_merge_patch` | sorts with one comparator but walks with plain `strcmp`, losing data when keys differ only by case |
| 22 | `generate_merge_patch` | recurses through the public case-insensitive wrapper, so the case-sensitive variant applies only at the top |
| 23 | `apply_patch` | removes the target before checking the rest of the operation, so a failed replace still destroys data |
| 24 | `overwrite_item` | copies the whole struct over a non-root node, wiping its sibling links and key |
| 25 | `Minify` | treats any backslash-quote as an escaped quote, ignoring backslash parity |
| 26 | `print_number`, `compare_double` | a relative-epsilon round-trip test accepts imprecise 15-digit output |
| 27 | `parse_hex4` | returns 0 for an invalid digit, the same as a real zero, so a bad `\u` escape is accepted |
| 28 | `parse_number` | no overflow/underflow check: out-of-range numbers become `inf` or 0 and print as `null` |
| 29 | `Compare` | always finds the first matching key, so identical objects with duplicate keys compare unequal; infinities too |
| 33 | `ParseWithOpts`, `parse_string` | advances past the opening quote before validating it, so the reported error position is off by one |

### How CeeLing solves it

**It does not.** A language that checks memory cannot know that `~1` should decode to `/`. The
port reproduces every one of these *exactly*, on purpose, because the differential test is only
meaningful if the port is the same program. That is the point of the table below: the same inputs
through upstream and through the port, side by side.

```
#15 add /a~1b (should make key a/b)    upstream: {"a~/":1}     port: {"a~/":1}     SAME
#17 pointer /2^64 wraps to index 0     upstream: 10            port: 10            SAME
#23 failed replace still removes       upstream: status 7 {}   port: status 7 {}   SAME
#24 root-replace on non-root member    upstream: {"a":1,"":9}  port: {"a":1,"":9}  SAME
#25 minify: backslash parity           upstream: {"a":"x\\","b": 1 }   port: (identical)  SAME
#28 1e999 / 1e-999                     upstream: [null,0,null] port: [null,0,null] SAME
#33 error position, bad key            upstream: error at 2    port: error at 2    SAME
        ... 18 cases in all: 18 identical, 0 different   (tests/vulns/logic_cases.cpp)
```

What the port *does* change is the **blast radius**. In C, the article reports that 14 and 24 also leak the
dropped nodes. In the port the dropped nodes are simply released, so the same wrong answer arrives
without the leak. That is a real improvement and a modest
one: the user still loses data.

## 19.8 What it took — the compiler bugs a real library found

The case study was also a test of CeeLing itself, and it failed that test several times before it
passed. Each item below was a real defect or gap found by porting cJSON, then fixed in the
compiler with a regression fixture in `tests/safety/`.

| Found while | The problem | The fix |
|---|---|---|
| writing `break`/`continue` | a use after a move carried across loop iterations was accepted, and the generated C++ crashed | loop bodies are re-analysed from the loop-head state |
| building a long list | the generated destructor recursed once per node and overflowed the stack at ~100,000 nodes | an iterative destructor for self-owning classes |
| writing library functions | a parameter that was only compared with `nullptr` was treated as *consumed*, so a null check took the caller's argument | `== nullptr` is a read |
| storing into fields | a parameter stored into a field (in a free function, in a nested block, through another object) was not shared | the promotion now covers all three |
| reusing a local name | pointer kinds were looked up by variable *name* across the whole program, so two functions' `item` locals disagreed | scope-aware lookup |
| a list walk | `last = node; ... node.x` was rejected: assigning a local moved it | two live handles to one object are both shared |
| a field named `prev` | a `//@ptr weak` hint leaked onto a *parameter* named `prev`, emitting `weak_ptr` and then `prev->next` | hints apply to fields only |
| reading a weak link through an included class | the null-check helper was only emitted into files that *declare* a weak field | emitted whenever one is in scope |
| writing the triggers | `double x = 1.0e30;` **compiled as `1.0`**: tree-sitter's error recovery silently dropped the stray `e30` | any syntax error is now a compile error; exponent literals are supported |
| the safety runner | a compiler *crash* counted as a successful "rejection" | exit codes ≥ 128 fail the fixture |
| measuring speed | every `string` argument was copied on every call | read-only string parameters pass as `const std::string&` |
| measuring speed | `malloc` per node made the parser 1.6x slower than C | an opt-in per-thread pooled allocator (`--pool-shared`) |

The `1.0e30` row is the one to remember. A *soundness* bug — the compiler accepted a malformed
program and changed its meaning without a word — was sitting in the front end, and it was found
because a vulnerability trigger needed a large constant.

## 19.9 Speed and memory, since "safe" is not free

With the checks on, the port parses at **86 MB/s against upstream's 81** and prints at 105 against 69; freeing a tree is
5x faster. Every kind of token is at parity or ahead except very long strings (6% behind on parse). The price is
**memory: 2.2x** (457 MB against 207 MB for the same 20 MB document), because a reference-counted node
with a weak back-link and two inline strings is 160 bytes against C's 64. Details and the history of how the
first 1.6x gap was closed are in `cjson-cling/PERFORMANCE.md`.

## 19.10 What this shows, and what it does not

**It shows:**

- Of the cJSON bugs that are *memory errors or crashes* (1–13), CeeLing removes 7 outright, converts 2 from
  silent corruption into a located abort, and leaves 3 partly open and 1 open. The mechanism is almost always the
  same: the dangerous operation is not in the language.
- It does so **without annotations**. The port contains no lifetime syntax; the one explicit hint is
  `//@ptr weak Item.prev`, which marks a back-link.
- A real library is a much harder test than the fixtures: it found ten compiler defects and two performance
  gaps, one of the defects a silent miscompilation.

**It does not show:**

- **That CeeLing makes code correct.** 17 of the 33 are logic errors and all 17 are still there.
- **That it bounds resource use.** Stack depth and CPU time are not checked (6b, 8, 9b, 10).
- **That the hand-written parts are safe.** The 30-line number shim, and the C++ standard library the
  generated code compiles against, are trusted.
- **Thread safety.** Shared mutable access across threads is out of scope by contract.
- **Anything about the author's exact exploits**, which were not used (see the caveat in 19.1).
- **That it is free of every memory error.** A sanitizer run shows no error on the inputs it saw (the corpus,
  1.5M mutations, the 776 checks, the 20 triggers); it is evidence, not a proof. The fuzzer is
  mutation-based and never generates, say, a hand-built cyclic tree on its own.

## 19.11 The full table

| # | cJSON function | Defect (in C) | In the port | Evidence |
|---|---|---|---|---|
| 1 | `overwrite_item` | frees fields ignoring reference flags | **cannot happen** | `01_overwrite_reference` |
| 2 | `ApplyPatches` | reads next patch after deletion | **cannot happen** | `02_patch_deletes_itself` |
| 3 | `merge_patch` | walks deleted nodes (aliasing) | **cannot happen** | `03_merge_patch_alias` |
| 4 | `ReplaceItem*` | frees item still referenced | **cannot happen** (+ library check) | `04_replace_with_ancestor` |
| 5 | `DetachItemViaPointer` | NULL `parent->child` write | **run-time abort** | `05_detach_null_child` |
| 6 | `ReplaceItemInArray` | array inside itself | **partly**: direct form refused, indirect cycle crashes | `06_`, `06b_` |
| 7 | `ParseWithOpts` | stale error pointer | **cannot happen** | `07_stale_error_pointer` |
| 8 | `Duplicate` | cap too high for the stack | **partly**: stack-dependent, no check | `08_`, `08b_` |
| 9 | `Delete`, patch `copy` | unbounded recursion | **partly**: delete fixed, print still recurses | `09_`, `09b_` |
| 10 | `Compare` | exponential time | **not prevented** | `10_compare_exponential` |
| 11 | `apply_patch` | NULL object dereference | **run-time abort** | `11_patch_null_root` |
| 12 | `apply_patch`, `detach_path` | NULL string value | **cannot happen** (no null strings) | `12_string_without_value` |
| 13 | `cJSON_Utils` allocs | unchecked `malloc` | **cannot happen** (no `malloc`) | `13_alloc_unchecked` |
| 14 | `sort_list` | later append dropped | same bug (leak gone) | `14_sort_then_add` |
| 15 | `decode_pointer_inplace` | `~1` wrong position | same bug | `logic_cases` #15 |
| 16 | `decode_pointer_inplace` | invalid escape ignored | same bug | `logic_cases` #16 |
| 17 | `decode_array_index...` | index wraps | same bug | `logic_cases` #17 |
| 18 | `decode_array_index...` | empty token is 0 | same bug | `logic_cases` #18 |
| 19 | `get_item_from_pointer` | no leading `/` → root | same bug | `logic_cases` #19 |
| 20 | `detach_path` | mixed case sensitivity | same bug | `logic_cases` #20 |
| 21 | `generate_merge_patch` | comparator mismatch | same bug | `logic_cases` #21 |
| 22 | `generate_merge_patch` | case-insensitive wrapper | same bug | `logic_cases` #22 |
| 23 | `apply_patch` | destroys before validating | same bug | `logic_cases` #23 |
| 24 | `overwrite_item` | wipes sibling links | same bug (leak gone) | `24_`, `logic_cases` #24 |
| 25 | `Minify` | backslash parity | same bug | `logic_cases` #25 |
| 26 | `print_number` | imprecise 15 digits | same bug | `logic_cases` #26 |
| 27 | `parse_hex4` | invalid hex is 0 | same bug | `logic_cases` #27 |
| 28 | `parse_number` | overflow unchecked | same bug | `logic_cases` #28 |
| 29 | `Compare` | duplicate keys, infinities | same bug | `logic_cases` #29 |
| 30 | `SetValuestring` | cross-allocation pointer compare | **cannot happen** | `30_set_valuestring_self` |
| 31 | `CreateNumber` | NaN to int | **avoided by the library** (`saturateToInt`) | `31_nan_to_int` |
| 32 | `Version`, `global_error` | shared static state | **cannot happen** (no file-scope variables) | `file_scope_variable_rejected` |
| 33 | `parse_string` | off-by-one error position | same bug | `logic_cases` #33 |

Run under AddressSanitizer + UBSan (`tests/run_sanitizers.sh`), the 20 memory-class triggers give: **clean** for
every "cannot happen" row; **the CeeLing null check fires** (abort, exit 134 under ASan) for 5 and 11;
**`AddressSanitizer: stack-overflow`** for exactly the two programs marked "still open" (06b, 09b). The sanitizer
independently agrees with the table.

## Reproducing it

```
cd cjson-cling
bash tests/run_vulns.sh        # the 20 memory-class programs (outcomes self-checked) + the 18 logic cases
bash tests/run_ported.sh       # 776 ported upstream unit-test checks
bash tests/run_diff.sh --fuzz 3000          # 1.5M differential parses vs upstream
bash tests/run_diff_utils.sh --fuzz 1000    # Utils: pointer / patch / merge patch / sort
bash tests/run_threads.sh      # 8 threads on the pooled allocator
bash tests/run_sanitizers.sh --fuzz 1500   # all of the above under ASan+UBSan, in WSL (~5 min)
bash tests/run_bench.sh 20     # speed and memory vs upstream
```

Source of the issue list: Joshua Rogers, *33 Vulnerabilities in cJSON*, 2026-07-25.
