# Writing a chapter of the language guide

Read [`01-why-ceeling.md`](01-why-ceeling.md) first: it is the model for tone, layout and for how examples are shown. The examples, their tags and the checker are described in
[`examples/README.md`](examples/README.md). This file says how a chapter is written.

The guide is a book: one file per chapter, in order, each readable from top to bottom. A chapter quotes its examples; it does not send the reader to a folder to find the point.

## Who reads it, and what they must get

A C++ programmer who has not seen CeeLing. For every feature they must come away knowing three things, **with a program in front of them**:

1. **The problem.** What goes wrong in C++, shown by a C++ program that really compiles (and, honestly, whether `g++ -Wall -Wextra` warns about it).
2. **Why C++ does not stop it.** One tight paragraph. Say "cannot" only when it is true of the language (a reference carries no lifetime; `std::move` is a cast). Where C++ merely *chooses* not
   to (unchecked `operator[]`), say "does not", and name the opt-in tools fairly (`-D_GLIBCXX_ASSERTIONS`, sanitizers, hardened libraries, clang-tidy): they exist, they are not on by default
   or not complete, and none of them is a compile-time guarantee about *your* code.
3. **What CeeLing does and why it can.** The rule in a few sentences, then the example, then **what it costs or where it stops**. CeeLing can do it because it is a smaller language that
   controls the whole pipeline (parser, analysis, code generation): it may forbid what C++ must allow, and it may insert code C++ would never insert.

Do not skip the last part of 3. Every section ends with what is not covered.

## Layout of a feature section

```
### Name of the feature
(one sentence: the problem it solves)

<!-- example: NN_the_bug.cpp -->        the C++ that compiles and is wrong (or merely clumsy)
**Why C++ does not stop it.** ...
**What CeeLing does.** ...
<!-- example: NN_the_feature.clg -->     accepted: add <!-- output: ... -->; rejected: add <!-- error: ... -->
(optional) <!-- generated: NN_the_feature.clg "int main()" -->   the C++ it became, when seeing it teaches something
**Where it stops.** ...
```

A feature that has no C++ problem behind it (a convenience such as `enum` or `namespace`) still says what C++ makes you write and what CeeLing writes for you; do not invent a bug.

## Honesty rules (the project's, not optional)

* Never write "Rust-equivalent", "thread-safe" or "memory-safe" without the exact scope. The scope is in `CLAUDE.md` ("The Safety Contract", "Current status"): the static half (dangling,
  use-after-move, sealed hatches), the run-time half (bounds, null), `spawn` and `Channel`; trusted: `ext_fn`, shims, libraries.
* **Verify before you write.** Anything older than the compiler is suspect: planning words ("Phase", "design target", "intended syntax"), `owned<T>`/`borrowed<T>` notation, features listed as
  missing that now exist, or the reverse. The compiler and `CLAUDE.md` are the authority. If a claim cannot be shown by an example or found in `CLAUDE.md` or `TODO.md`, leave it out or say it is
  not verified. When the compiler and the documents disagree, say so; do not smooth it over.
* Numbers only with their source (a measurement in `CLAUDE.md` or `TODO.md`, or something that was run). No dates, no "now", no version diaries: the guide describes the language as it is.
  Development history belongs in `docs/internals/`.
* No marketing, no emojis, short sentences. Tables when comparing; prose when explaining.

## Chapters

`01-why-ceeling` `02-language` `03-values-and-containers` `04-ownership` `05-moves` `06-dangling-references` `07-run-time-checks` `08-sealed-escape-hatches` `09-closures` `10-threads`
`11-talking-to-cpp` `12-many-files` `13-contract-and-threats` `14-how-it-works` `15-case-study-cjson` `16-case-study-app-cling` `17-comparisons` `18-status`, each `.md`, in this folder. Link to chapters by these names.
An example's name starts with its chapter's number: `07_bounds_read.clg`.

## Before a chapter is finished

`check.py write --chapters <chapter files>` and then `check.py check <NN_> --chapters <chapter files>` must report 0 problems. Re-read each section and ask: does the reader know the problem,
why C++ left it open, why CeeLing can close it, and where it does not?
