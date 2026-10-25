# 20. Threads — Only Values Cross

Chapters 2–4 draw the line at one thread: *"the contract guarantees single-threaded memory
safety only."* That is still true of the language as a whole. This chapter documents the one
deliberate exception: **a thread can be started, and the compiler checks exactly what is handed
to it.** It does not make CeeLing thread-safe, and the last section lists what it leaves open.

## The problem in raw C++

Starting a thread is easy. Handing it data safely is not, because C++ lets a thread capture
anything:

```cpp
std::shared_ptr<Counter> c = std::make_shared<Counter>();
std::thread t([c] { c->n++; });   // the thread holds a copy of the handle ...
c->n++;                           // ... and so does this thread: a data race on one object
t.detach();
```

The capture is "by value", so it looks safe. But a by-value copy of a `shared_ptr` is a second
handle to the **same object**, not a copy of the object. Chapter 4's closure rules (by-value
capture only, `[&]` unrepresentable) close the dangling-reference hole and say nothing about this
one. A naive `spawn` that accepted any closure would have shipped a *false sense of safety*:
"the closure was already verified" would be true and irrelevant.

## How CeeLing solves it

The rule is deliberately narrow: **a spawned job can reach only memory that no other thread can
reach.** That leaves three things a thread may be given.

```
class Job { public: Job() { } virtual void run() { } };   // any class whose one virtual method is void run()

spawn(closure Job [n, word, data] () { ... });             // runs on a new thread
```

| Captured | What happens | Why it is safe |
|---|---|---|
| primitive, `string`, `std::vector` of those | **copied** | the thread has its own copy; nothing is shared |
| a `struct` made only of those (base struct included) | **copied** | a struct is stored bare, with no handle, so it cannot hide a subclass |
| a flat class object (five conditions below) | **moved** | the creating thread can no longer name it |
| anything else | **compile error** | — |

Everything crosses the boundary as a value or as an object that has *stopped existing* on the
creating side. Data that does not fit travels as a `string` (JSON, for instance) and is rebuilt
inside the thread — the same advice as Chapter 19's cJSON tree, which has `weak` parent links and
so can never be moved.

### When can an object be moved?

An object is handed over only if the compiler can show that nothing else can reach it **and**
that nothing about it is hidden. All five must hold:

1. **The class is flat and closed.** Every field is a plain value (no class-typed field), it is
   defined in this file, has no base class, is inherited by no class in the program, has no
   virtual method, and never hands out `this`.
2. **Its dynamic type is exactly the class.** The captured variable is a local of the capturing
   function created *only* by `new T(...)` — not a function result, a parameter, a field or
   another variable. This is the answer to "a base-typed handle might hold a subclass, defined in
   another file, with shared fields": the compiler cannot see other files, so it requires proof
   instead.
3. **It has one owner.** The smart-pointer inference decided `unique`. If the object is also
   stored in another object, the inference makes it `shared` and the move is refused.
4. **It is moved-from afterwards.** The ordinary use-after-move rule (Chapter 12) now applies:
   touching the object after `spawn`, or spawning an object declared outside a loop from inside
   it, is an error.
5. **The job holds it uniquely.** A spawn job is the one class where the inference does *not*
   apply "a parameter stored into a field is linked, so both become shared"; the generated
   constructor `std::move`s the capture into a `unique_ptr` field, and the checker confirms the
   inference agrees.

### Other things that are refused

- `spawn(obj)` — only a closure *literal* is accepted, so the capture list is the one place that
  needs checking.
- A job whose interface method is not `void run()` (no caller to return to).
- A `spawn` inside a spawned body (no unbounded thread multiplication in v1).
- A job body that names an enclosing local it did not capture.
- A hand-written call to the runtime helper `cling::spawn`.

## Proof by example

A flat object, moved:

```
class Buffer {
public:
    std::vector<int> data;
    int n;
    Buffer() { n = 0; }
    void add(int x) { data.push_back(x); n = n + 1; }
    int sum() { /* ... */ }
};

Buffer b = new Buffer();
b.add(1); b.add(2); b.add(3);
spawn(closure Job [b] () {
    std::cout << "sum " << b.sum() << " n " << b.n << std::endl;   // prints: sum 6 n 3
});
```

generated C++ (abridged):

```cpp
__closure_0::__closure_0(std::unique_ptr<Buffer> p_b) : b(std::move(p_b)) { }
...
cling::spawn(std::make_shared<__closure_0>(std::move(b)));
```

No copy of the data, a `unique_ptr` inside the job, and `b` is gone on the creating side.

What the compiler says to the dangerous versions (these are the real messages):

```
// the object is also stored elsewhere, so it has two owners:
error: `spawn` cannot move `mine` into a new thread: `mine` has more than one owner (it is
       stored elsewhere or shared), so moving it would leave a live alias in this thread

// the object came from a function call; its dynamic type is unknown here:
error: `spawn` cannot move `b` into a new thread: `b` is not created only by `new Buffer(...)`
       in this function, so it might hold a subclass object or an object also reachable from elsewhere

// the class has a class-typed field:
error: `spawn` cannot move `o` into a new thread: field `inner` of class `Outer` is not a plain
       value (type `Inner`); only primitives, string, vectors of those and plain structs are allowed

// the creating thread keeps using the object:
error: use of moved-from value `b`

// the body reaches a local it did not capture:
error: the `spawn` body uses `hidden`, which belongs to the enclosing scope but is not in the capture list
```

The first three print as `error[semantic/spawn-captures-object]`; the fourth is the ordinary
`semantic/use-after-move`; the last is `semantic/spawn-uncaptured-name`.

The fixtures are `tests/safety/spawn_*.clg` (27 of them; 6 accepted, with `//@ run:` where the
claim is a runtime property such as "the worker really got a copy" or "a throwing job does not
abort the process").

## What the runtime does

`cling::spawn` is a small helper emitted into the generated file. It starts one `std::thread`
per job and keeps the threads in a registry that is **joined when the process exits**, so a job
cannot be lost by `main` returning. A job that throws is reported on stderr and its thread ends;
the exception never reaches `std::terminate`. Nothing is pooled and nothing is capped.

## A worked use: app-cling

The desktop-app framework `app-cling` (a window plus CeeLing commands) runs slow commands on
workers. The command body runs on the main thread, copies what it needs into plain values,
spawns, and the worker answers the web page itself:

```
app.asyncCommand("scan", closure AsyncCommand [] (Item args, string id, int handle) {
    string payload = printUnformatted(args);          // the JSON tree stays here ...
    spawn(closure Job [payload, id, handle] () {
        Item mine = parse(payload);                   // ... the worker builds its own
        replyOk(handle, id, printUnformatted(mine));
    });
});
```

The `Item` tree cannot be captured (it has `weak` parents, so it is shared), which is exactly why
the example passes a string. Replies go through a mutex-protected queue in the C++ shim — trusted
code, outside the guarantee.

## How it was tested

- **Compiler:** 27 `tests/safety/spawn_*` fixtures; the full safety suite is green (263).
- **Real threads:** app-cling's 27 end-to-end tests start real workers: out-of-order replies,
  50 concurrent jobs, a struct copied in (the main thread changes its own copy afterwards and the
  worker still sees the original), an object moved in.
- **Sanitizers:** the whole suite under AddressSanitizer + UndefinedBehaviorSanitizer and under
  ThreadSanitizer, with and without the pooled allocator: no reports.
- **Negative controls:** the same ThreadSanitizer run against a shim whose mutex has been removed
  (on the call queue; on the reply path) *must* report races, and does (16 and 40 reports in the
  last run). A sanitizer run that cannot fail proves nothing; this one can.
- A false alarm worth knowing about: GCC 11's ThreadSanitizer does not understand the clock wait
  behind `condition_variable::wait_for` and reports a "double lock" that is not there. The shim
  uses a `system_clock` deadline under `__SANITIZE_THREAD__` to avoid it.

## What this does not claim

The exact claim: **in CeeLing code that was checked, a spawned job can reach only its own
copies, objects moved into it, and its own allocations, so there are no data races through
`spawn`.** It is *not* "thread-safe", and these remain open:

- **`ext_fn` and shim code.** A worker may call a trusted function that touches shared state;
  the compiler cannot see inside it.
- **Shared state outside CeeLing's memory:** files, sockets, the window.
- **Deadlock and starvation.** There are no locks in the language, but a worker can block forever,
  and because exit joins every worker, that blocks exit.
- **Number of threads.** One per `spawn`, no cap, no pool.
- **A failed job leaves its caller waiting.** A throw ends the thread; if the job was meant to
  answer a request, nothing does, unless the job catches its own error and replies.
- **Hidden state in a trusted type.** Objects are checked by their fields' types; a class built on
  a library type whose internals share state is not looked into.

## What is deliberately not here

Rust's `Send`/`Sync`, exclusivity tracking and `thread::scope` need lifetimes, which Chapter 6
explains CeeLing does not have. Objects with object-typed fields, objects from headers or from
function results, `Channel<T>`, a thread pool and cancellation are listed in `TODO.md` under
Concurrency Support. Whoever needs the strongest guarantee or full data-race freedom should use
Rust.
