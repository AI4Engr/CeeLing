# 14. Null Dereference

"All smart pointers" does **not** mean "never null." A `shared_ptr` or `unique_ptr` can be
empty, and `weak_ptr::lock()` can fail. This is as high-frequency as bounds checking.

> **Status: implemented (weak links 2026-10-19, every object handle 2026-10-24).** An earlier
> version of this chapter described non-null typing "where provable". CeeLing does no such
> analysis: every dereference is checked. The chapter now describes what the compiler does.

## The problem in raw C++

```cpp
std::shared_ptr<Node> p;         // default-constructed: empty
p->foo();                        // null dereference, UB

std::weak_ptr<Node> w = prev;
w.lock()->foo();                 // if the object is gone, lock() is empty: UB
```

## What CeeLing does

- **Every `->` the generator produces** (member access, method call, class-typed `operator[]`)
  goes through `cling::nn(handle, "line N")`. An empty handle prints
  `cling: null dereference at line N` and calls `abort()` (exit status 3). It is `abort`, not
  `throw`, so user code cannot swallow it. `nn` forwards its argument, so an lvalue handle is not
  copied (no reference-count traffic).
- **A `weak_ptr` field (Rule C, e.g. `prev`) is read through `.lock()`**: `owner` yields a
  `shared_ptr` that may be empty, so `!owner`, `owner == nullptr` and `if (owner)` compile, and a
  member access through it is checked like any other. Writes to the field are plain stores.

```
Node n = maybeGet();             // may return empty
std::cout << n.key;              // empty: "cling: null dereference at line 12", exit 3
```

There is no "provably non-null" case that skips the check; the cost was not measurable on the
cJSON port (parse 50.0 MB/s with and without).

## Turning it off

On by default; `--no-null-checks` removes it for one compile (zlib-cling is built without; the
cJSON port with).

## What it does not cover

- `this` is not checked.
- A handle passed to an `ext_fn` is not inspected (trusted boundary).
- It finds an EMPTY handle. It does not find a `ref` observer that dangles: `ref` lowers to a raw
  pointer whose safety rests on the static dangling analysis (making observers self-checking is an
  open item in `TODO.md`).

Tests: `tests/safety/null_deref_field_runfail`, `null_deref_returned_runfail`, `null_checked_ok_accepted`,
and the weak-field fixtures.
