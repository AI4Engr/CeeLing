# 13. Out-of-Bounds Access

This is the first **runtime-backstop** chapter: some safety cannot be proven
statically without over-rejecting, so CeeLing inserts a runtime check.

> **Status: implemented (2026-10-19).** An earlier version of this chapter described a
> design (a check only where the index is not provable, a compile-time rejection of a
> constant index, a catchable panic). The implementation is simpler and stricter in
> one way and weaker in another; this chapter now describes what the compiler does.

## The problem in raw C++

Indexing past the end is undefined behavior, silently:

```cpp
std::vector<int> v = {1, 2, 3};
int x = v[5];            // UB: reads arbitrary memory, no error
v[10] = 7;               // UB: corrupts memory
```

Whether `i` is in range often depends on runtime values a static checker cannot know,
so pure static rejection would either miss real bugs or reject mountains of valid code.

## What CeeLing does

**Every subscript on a `std::vector` or `string`, read or write, is checked at run time.**
`v[i]` is emitted as `cling::at(v, i, <source line>)`:

- one unsigned comparison, so `i >= size` and every negative `i` are caught by the same test;
- on failure the program prints `cling: index out of bounds at line N (index I, size S)` to
  stderr and calls `abort()` (exit status 3 on the reference toolchain);
- it is `abort`, not `throw`: a `catch (string e)` in the program cannot swallow it;
- `std::map` subscripts keep their insert-or-update meaning (the helper tells them apart by
  type), so they are not "out of bounds" by definition;
- the helper is emitted only into files that contain a subscript.

There is **no static elimination**: CeeLing does not prove any index in range, and a constant
index past the end is not a compile error. (g++ may remove checks it can prove redundant.)

```
std::vector<int> v;
v.push_back(1);
int i = readIndex();         // unknown at compile time
std::cout << v[i];           // i == 5 at run time: "cling: index out of bounds at line 4 (index 5, size 1)", exit 3
```

## Turning it off, and what it costs

On by default. `--no-bounds-checks` removes it for one compile. Measured on zlib-cling
(12-file Silesia corpus): **+5.5% decompress, +11.3% compress**; zlib-cling is built with the
checks OFF to keep speed parity with zlib-rs, and a checked build is one flag away. The
cJSON port is built with them ON. What a claim about a program means depends on which.

## What it does not cover

- `pop_back()` on an empty vector, `front()` / `back()`, and iterators moved past `end()`.
- `std::string::substr` with a bad position: libstdc++ throws `std::out_of_range`, which
  `catch (string e)` does not catch, so the program terminates with an unhandled exception.
- Anything an `ext_fn` shim indexes: that code is outside CeeLing's analysis (trusted boundary).
- The failure message names the line, not the file.

A note for anyone measuring "unchecked" C++: libstdc++ turns on its own `operator[]` assertions in
UNOPTIMIZED builds only, so the same source aborts at `-O0` and reads garbage at `-O2`. That is
why the check is inserted by the compiler instead of being left to the user's flags.

Tests: `tests/safety/bounds_*` (read, write, negative index, string, in-range and `std::map` accepted).
