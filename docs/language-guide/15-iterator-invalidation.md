# 15. Iterator Invalidation

The **highest-frequency C++-specific dangling source** — one that Java does not have
(Java fails fast with a `ConcurrentModificationException`; C++ gives silent UB).

## The problem in raw C++

Mutating a container can invalidate iterators, pointers, and references into it. Using
them afterward is undefined behavior:

```cpp
std::vector<int> v = {1, 2, 3};
auto it = v.begin();
v.push_back(4);          // may reallocate the buffer → `it` now dangles
*it;                     // ← UB

int& r = v[0];
v.push_back(4);          // realloc → `r` dangles
r = 9;                   // ← UB

for (auto it = v.begin(); it != v.end(); ++it)
    if (*it == 2) v.erase(it);   // ← erase invalidates `it`, then ++it is UB
```

This is insidious because the code looks obviously correct and often "works" until the
vector happens to reallocate.

## How CeeLing solves it

An iterator/reference into a container has **provenance into that container**
(see [Chapter 5](05-how-analysis-works.md)). The library summary table marks which
methods **invalidate** which iterators:

```
std::vector<T>::push_back(self: &mut, ...)   invalidates iterators of self (on realloc)
std::vector<T>::erase(self: &mut, it)        invalidates iterators at/after it
std::vector<T>::clear(self: &mut)            invalidates all iterators of self
std::vector<T>::begin(self: &)  -> iterator borrowing self
```

An **invalidating call is a death event** for every live iterator/reference whose
provenance is that container. Any subsequent use of such an iterator is **rejected**.

## Proof by example

### Rejected — use after invalidation

```
Vector<int> v = {1, 2, 3};
Iter it = v.begin();     // provenance: it <= v
v.push_back(4);          // summary: push_back invalidates iterators of v → it DEAD
std::cout << deref(it) << std::endl;        // ❌ REJECT: use of invalidated iterator `it`
```

### Rejected — erase-in-loop

```
for (Iter it = v.begin(); it != v.end(); it = next(it)) {
    if (deref(it) == 2) v.erase(it);   // ❌ REJECT: `it` invalidated by erase, then reused
}
```
Fix the analyzer suggests: use the value returned by `erase`, or collect-then-remove.

### Accepted — re-fetch after mutation

```
Vector<int> v = {1, 2, 3};
v.push_back(4);          // mutate first
Iter it = v.begin();     // fetch AFTER the mutation → provenance fresh → ✅ ACCEPT
std::cout << deref(it) << std::endl;
```

The same mechanism that catches dangling references catches iterator invalidation —
it is just a death event sourced from a library summary rather than a scope exit.
