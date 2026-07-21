# 13. Out-of-Bounds Access

This is the first **runtime-backstop** chapter: some safety cannot be proven
statically without over-rejecting, so CeeLing inserts a runtime check — exactly as
Java always does and Rust does for indexing.

## The problem in raw C++

Indexing past the end is undefined behavior, silently:

```cpp
std::vector<int> v = {1, 2, 3};
int x = v[5];            // ← UB: reads arbitrary memory, no error
v[10] = 7;               // ← UB: corrupts memory

int a[3];
for (int i = 0; i <= 3; ++i) a[i] = i;   // ← off-by-one, writes a[3] — UB
```

Whether `i` is in range often depends on runtime values a static checker cannot know,
so pure static rejection would either miss real bugs or reject mountains of valid
code.

## How CeeLing solves it

**Static where provable, runtime check otherwise.**

- When the index is a provable constant/range within bounds, no check is emitted.
- Otherwise the generator inserts a bounds check that **panics/throws** on violation —
  a defined, catchable failure instead of silent UB.

Conceptually, `v[i]` lowers to a checked access:

```cpp
// generated
__cl_at(v, i)   // ≈ if (i >= v.size()) __cl_panic("index out of bounds"); return v[i];
```

(`.at()` is the std building block for the runtime path; the point is that the check
is *inserted for you*, not left to discipline.)

## Proof by example

### Rejected at compile time (provably out of range)

```
int[3] a;
a[3] = 1;                // ❌ REJECT: index 3 out of bounds for length 3
```

### Runtime-guarded (index not statically known)

```
Vector<int> v = {1, 2, 3};
int i = readInt();       // value unknown at compile time
std::cout << v[i] << std::endl;             // ✅ compiles; if i >= 3 at runtime → panic, not UB
```

Result: an out-of-bounds access can never silently corrupt memory — it is either
caught at compile time or turned into a clean runtime panic.
