# 17. Sealed Escape Hatches

Safety is a **closure property**: one open hole voids the entire contract. If a user
can obtain a raw pointer, bypass the type system, or read uninitialized memory, then
all the analysis in the preceding chapters is decorative. This chapter is short but
foundational — and in the roadmap it is implemented **first**
([Phase 1](09-status-and-roadmap.md)), because it is both the simplest and the most
load-bearing. **Status: implemented.** Every hatch below is enforced today, with a
worked diagnostic.

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

**None of these are C++ bugs.** Every one compiles cleanly under any C++ compiler: raw
pointers, casts, and `union` are core, intentional language features, and `.get()` is
a normal, well-typed standard-library API. C++ has no "safe subset" switch — turning
any of these off would break an enormous amount of existing code, so the standard
cannot forbid them, only document that misusing them is undefined behavior and trust
the programmer.

## Why CeeLing can do what C++ can't

CeeLing's mechanism is different **in kind**, not degree. It does not try to detect
*misuse* of these features at the type-system level — C++ itself cannot do that
without breaking backward compatibility, which is exactly why `-Wall -Wextra` still
lets all six examples above through. Instead, CeeLing **removes them from the
grammar's acceptance surface entirely**: there is no production in
`tree-sitter-ceeling/grammar.js` for a pointer declarator, a cast expression, `delete`,
or `union`. A program containing one of these constructs is not "C++ code CeeLing
analyzes and flags" — it is not a program in the CeeLing language at all, in the same
sense that `x =+ 1;` is not a Python program. This is the same **escape-by-construction**
principle already used for the observer-parameter ban (Chapter 11: `ref` cannot appear
as a function parameter because the grammar has no production for it there), applied
to a different hazard class.

The one genuine wrinkle: two of these hatches (`.get()` and casts) don't actually
produce a hard parse error on their own, because CeeLing reuses ordinary C++-looking
syntax for other things. `.get()` parses as an ordinary method call — indistinguishable
at the grammar level from a user's own `Box.get()` accessor. `reinterpret_cast<int>(x)`
parses as a chain of `<`/`>` comparisons (`reinterpret_cast < int > (x)`), since
CeeLing has no cast syntax of its own to claim those tokens. Both are caught by a
**semantic** check instead (`EscapeHatchChecker`, see [Chapter 18](18-implementation-notes.md))
that runs on the validated AST before code generation — still "rejected before a
single line of C++ is emitted," just one layer later than a grammar-level rejection.

**This is also why the AST-driven generator had to become the default** (Chapter 18,
Step 7): the property "a rejected construct can never reach the emitted C++" is only
true if the generator *builds* its output from the same validated AST that was
checked. CeeLing's original generator built output by regex-substituting the raw
source text — so even with every check in this chapter passing, `Node* p;` or
`delete a;` would still appear verbatim in the generated file, because the generator
never looked at the AST for that part of the output. Sealing the hatches at the
analysis layer is necessary but not sufficient; the generator has to actually honor
the rejection.

## How CeeLing solves it

| Hatch | CeeLing | Mechanism |
|-------|---------|-----------|
| Raw pointer `T*` | **forbidden** | semantic: `ClassName * identifier` can never be real multiplication |
| Raw `new` / `delete` | **forbidden** (allocation is compiler-generated) | `delete` has no grammar production — parse error |
| `.get()` on a smart pointer | **forbidden** — raw pointers have nowhere legal to go | semantic: receiver resolves to a CeeLing object AND the class has no own `get()` |
| `reinterpret_cast`, C-style cast | **forbidden** | semantic (identifier name) + parse error (malformed comparison chain) |
| `const_cast` | **forbidden** | same as above |
| Raw `union` | **forbidden**; `variant` guarding not yet implemented | no grammar production — parse error |
| Uninitialized read (locals) | **rejected** — definite-initialization required | `variable_declaration` grammar requires `= expression` |
| Unknown library call (no summary) | not yet implemented | tracked under Phase 2 |

## Proof by example

### Rejected — the `.get()` back door

```
Node p = new Node(1);
int r = p.get();
```
```
error[semantic/forbidden-construct]: '.get()' is forbidden on a CeeLing object ('p' is a Node)
  --> file.clg:2:13
  help: '.get()' would extract a raw pointer, which has no legal use in CeeLing (CLAUDE.md
        hatch #14) -- use the object directly, or add a named accessor method to the class
        if you need a specific field.
```
Without this rule, a single `.get()` reintroduces a raw pointer and every later
dangling check can be bypassed — so sealing it is what makes Chapters 11–16 mean
anything. A class that defines its own `get()` method is unaffected — see
`tests/safety/get_on_user_method_accepted.clg`.

### Accepted — a user's own `get()` method

```
class Box {
public:
    int v;
    Box(int x) { v = x; }
    int get() { return v; }   // ordinary method, not a smart-pointer accessor
};
int main() {
    Box b = new Box(1);
    int r = b.get();          // OK: Box declares its own get()
    ...
```
The check distinguishes the two cases by whether the receiver's class *declares its
own* `get()` — only the compiler-generated smart-pointer accessor is a back door.

### Rejected — raw pointer declaration

```
Node* p;
```
```
error[semantic/forbidden-construct]: raw pointer types (`T*`) are forbidden (found `Node*`)
  --> file.clg:2:5
  help: pointers have no legal path through CeeLing (CLAUDE.md hatch #15); declare a
        plain object variable and let the compiler choose the smart-pointer type.
```
CeeLing has no separate pointer-declarator syntax, so `Node* p` is grammatically a
multiplication expression `Node * p`. The check catches this because a *class name*
can never be a legitimate left operand of multiplication — no real CeeLing program
multiplies a type by a variable, so the appearance of that shape is conclusive
evidence of an attempted pointer declaration.

### Rejected — manual delete

```
Node p = new Node(1);
delete p;
```
```
Error: Parse error at line 2, column 5, node type 'ERROR', near: delete p; Suggestion:
raw `delete` is forbidden (hatch #15) -- object lifetime is managed automatically by
the compiler-chosen smart pointer; there is no manual delete.
```

### Rejected — type confusion

```
int x = reinterpret_cast<int>(1.0);
```
`reinterpret_cast`, `const_cast`, `static_cast`, and `dynamic_cast` can never be legal
CeeLing identifiers (no class, variable, or function may be named after a cast
keyword), so their appearance is conclusive evidence of an attempted cast — caught
semantically where the surrounding expression happens to parse, and as a plain parse
error where it doesn't (the malformed comparison chain these tokens produce often
fails to build at all).

### Rejected — raw union

```
union U { int i; float f; }
```
```
Error: Parse error ... Suggestion: raw `union` is forbidden (hatch #17) -- it allows
reading a different member than was written, which is undefined behavior C++ cannot
catch. Use a class with distinct fields instead.
```

### Rejected — uninitialized read

```
int n;                   // declared, not initialized
```
`variable_declaration` requires `= expression` in the grammar — a bare local
declaration with no initializer has no production, so it is a parse error rather than
a program CeeLing can accept and then flag. (Class *fields* may be declared without
an initializer, e.g. `Node next;` — this is safe because the emitter always
default-constructs the underlying smart pointer to empty, never leaving raw C++-side
memory uninitialized.)

## Why this comes first

A leaky boundary makes later analysis meaningless: it is pointless to prove a
reference never dangles if the user can conjure a raw pointer with `.get()` and dangle
that instead. Seal the perimeter, *then* fill in the interior — every increment after
that is genuinely safe rather than safe-looking.
