# 17. Sealed Escape Hatches

Safety is a **closure property**: one open hole voids the entire contract. If a user
can obtain a raw pointer, bypass the type system, or read uninitialized memory, then
all the analysis in the preceding chapters is decorative. This chapter is short but
foundational — and in the roadmap it is implemented **first**
([Phase 1](09-status-and-roadmap.md)), because it is both the simplest and the most
load-bearing.

## The problem in raw C++

Every one of these is a legal C++ way to escape safety:

```cpp
auto p = std::make_shared<Node>();
Node* raw = p.get();     // (1) raw pointer back door — outlives p, dangles freely
delete raw;              // (2) manual delete → double free with p's destructor

int* q = reinterpret_cast<int*>(&someFloat);   // (3) type confusion
*q = 0;

const Config& c = getConfig();
const_cast<Config&>(c).mutate();               // (4) write through const → UB

union U { int i; float f; } u;
u.i = 1; float x = u.f;                         // (5) read wrong member → UB

int n;                                          // (6) uninitialized
use(n);                                         // reads garbage
```

## How CeeLing solves it

**Forbid them outright** — they are simply not part of the language subset.
Default-reject means "not provably safe ⇒ rejected," and each of these is unprovable
or actively unsafe:

| Hatch | CeeLing |
|-------|---------|
| Raw pointer `T*` | **forbidden** |
| Raw `new` / `delete` | **forbidden** (allocation is compiler-generated) |
| `.get()` on a smart pointer | **forbidden** — raw pointers have nowhere legal to go, so `.get()` has no purpose |
| `reinterpret_cast`, C-style cast | **forbidden** |
| `const_cast` | **forbidden** |
| Raw `union` | **forbidden**; `variant` guarded |
| Uninitialized read | **rejected** — definite-initialization required |
| Unknown library call (no summary) | **rejected** — default-reject |

## Proof by example

### Rejected — the `.get()` back door

```
Node p = new Node(1);
RawPtr r = p.get();      // ❌ REJECT: `.get()` is forbidden (raw pointer escape)
```
Without this rule, a single `.get()` reintroduces a raw pointer and every later
dangling check can be bypassed — so sealing it is what makes Chapters 11–16 mean
anything.

### Rejected — manual delete

```
Node p = new Node(1);
delete p;                // ❌ REJECT: raw `delete` is forbidden; cleanup is automatic
```

### Rejected — type confusion

```
float f = 1.0;
int x = reinterpret_cast<int>(f);   // ❌ REJECT: reinterpret_cast is forbidden
```

### Rejected — uninitialized read

```
int n;                   // declared, not initialized
std::cout << n << std::endl;                // ❌ REJECT: `n` read before initialization
```

## Why this comes first

A leaky boundary makes later analysis meaningless: it is pointless to prove a
reference never dangles if the user can conjure a raw pointer with `.get()` and dangle
that instead. Seal the perimeter, *then* fill in the interior — every increment after
that is genuinely safe rather than safe-looking.
