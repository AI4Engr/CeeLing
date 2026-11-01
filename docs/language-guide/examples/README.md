# The examples of the language guide

Every program the guide shows is a file in this one folder. A name starts with the number of the chapter that shows it: `04_shared_owner.clg` is a CeeLing program in chapter 4, and
`04_shared_owner.cpp` is its C++ twin (the same idea written in C++, which compiles and is wrong, or clumsy). The chapters quote these files; a tool checks that they say the same.

## Running them

```
docs/language-guide/examples/run.sh                     # all of them, one line each
docs/language-guide/examples/run.sh 04_shared_owner     # one: the source, what the compiler said, what the program printed
docs/language-guide/examples/run.sh 04_shared_owner.cpp # the C++ twin, compiled with g++ -Wall -Wextra and run (expect nonsense or a crash: that is the point)
```

An accepted `.clg` file is compiled to C++ with `build/cling.exe`, then by `g++`, and run. A rejected one stops at the compiler and prints its message. Set `CLING=/path/to/cling` for
another compiler.

## Checking them (and the chapters)

```
python3 -I docs/language-guide/examples/check.py check           # every example does what its tags say; every code block in the chapters equals its file; every file is shown somewhere
python3 -I docs/language-guide/examples/check.py write           # refresh the marked blocks of the chapters from the files and from what the compiler really printed
python3 -I docs/language-guide/examples/check.py check 04_ --chapters 04-    # only the examples whose name starts with 04_, and only the chapter files whose name contains 04-
```

The check is a CTest test (`cling_guide_examples`), so a change to the compiler that alters a message quoted in the guide, or makes an example stop compiling, fails the build.

## Tags (the first lines of a file, as comments)

| File | Tag | Meaning |
|---|---|---|
| `.clg` | `//@ expect: accept` or `reject` | required: does the compiler accept the program? |
| `.clg` | `//@ run: TEXT` | accepted; the program must print exactly these lines (one tag per line) |
| `.clg` | `//@ run-fail: TEXT` | accepted; the program must exit non-zero and its stderr must contain TEXT (a run-time check firing) |
| `.clg` | `//@ error: TEXT` | rejected; the compiler's message must contain TEXT |
| `.clg` | `//@ generated: TEXT` | accepted; the generated C++ must contain TEXT |
| `.clg` | `//@ flags: ...` | extra compiler flags |
| `.clg` | `//@ gdb: COMMAND` | repeatable, in order: the example (compiled with `//@ flags: -g`) is built with `g++ -g -O0` and run under gdb in batch mode, `tools/cling.gdb` sourced first; skipped if gdb is not installed |
| `.clg` | `//@ crash: TEXT` | accepted; the program must die abnormally (not exit 0 or 3); TEXT says how, for the reader |
| `.clg` | `//@ exit: N` | accepted; the program must exit with N (default 0) |
| `.clg` | `//@ project: cjson-cling` | the example uses a library from a sibling project (looked for next to this repository, or in `GUIDE_PROJECT_CJSON_CLING`); built with that project's `tests/build.sh`. Without the project the example is reported as SKIPPED, not failed |
| `.clg` | `//@ with: FILE ...` | an example of several files (repeatable). Everything is copied, without its `//@` lines, into one work directory; each listed `.clg` is compiled with `--cpp X.cpp` and `--cpp X.h`, a listed `.cpp` (a shim, a C++ consumer of generated code) is compiled as it is, a listed `.h` is just there to be `#include`d, and all of it is linked with the main file; `run` then runs the linked program. The main file alone decides accept or reject. A library file that has no `main` may list the program that uses it (or nothing, and is only compiled). `run.sh` does not build these: use `check.py` |
| `.clg` | `//@ demands: yes` | with `with`: the files must first FAIL to build together in g++ (the problem), then fit when built with `--ptr-demands` from what the main file writes with `--ptr-demands-out` (the fix) |
| `.c` | `//@ c: compiles`, `//@ project: cjson-cling`, `//@ sources: cJSON.c cJSON_Utils.c`, plus `run` / `crash` / `exit` | a C program built with gcc against the sibling project's `upstream/` sources (a C twin of a CeeLing trigger) |
| `.py` | (none) | a generator: prints a table a chapter quotes (`<!-- command: NAME.py -->`); exit 77 means a prerequisite is missing: skipped |
| `.cpp` | `//@ cpp: compiles` | required: `g++ -std=c++17 -Wall -Wextra -fsyntax-only` accepts it |
| `.cpp` | `//@ cpp: companion` | instead of `compiles`: built only through the `.clg` example that lists it in `with` |
| `.cpp` | `//@ warns: none` or `some` | required: whether g++ says anything at all, so that the guide's "the compiler is silent" is checked |

The tags are not shown in the chapters, and they are not compiled either: the checker (and `run.sh`) compiles a copy of the example without its `//@` lines, so a line number in a compiler message or a run-time check
is the line the reader counts in the chapter. A `generated` marker whose text starts with `line:` (`"line: int f(int x);"`) shows just the one line that starts with the rest (a declaration).

## Showing an example in a chapter

```
<!-- example: 04_shared_owner.clg -->          the source
<!-- error: 04_dangling.clg -->                 what the compiler printed (a rejected example)
<!-- output: 04_shared_owner.clg -->            what the program printed (an accepted example); for a run-fail example, its stderr
<!-- generated: 04_shared_owner.clg "int main()" -->   a piece of the generated C++: from the line that starts with that text to the closing } or }; at column 0
<!-- header: 12_lib.clg "class Counter {" -->      like generated, from the `.h` files (the main file's, then each `with` file's) of a multi-file example
<!-- demands: 12_use.clg -->                    what --ptr-demands-out wrote (a `demands: yes` example)
<!-- cxxerror: 12_use.clg -->                   the first g++ error of the files built without demands (a `demands: yes` example)
<!-- gdb: 11_debug_session.clg -->            the cleaned output of that gdb session (addresses and thread chatter removed)
<!-- stderr: 04_x.clg -->                       the program's stderr
<!-- command: 16_table.py -->                   the output of a generator script
<!-- source: cjson-cling/upstream/cJSON.c "static void foo" -->    a piece of a real file of a sibling project, from the first line containing the text to the line that closes it
<!-- source: cjson-cling/src/cjson_item.clg "Item foo" 30 -->    the same, cut after 30 lines
```

Each marker is followed by an ordinary fenced code block. Write anything in it and run `check.py write`: the block is replaced by the real thing.

## Writing a new example

Keep it under about forty lines, one idea, self-contained. Prefer a program that prints something, with `//@ run:`. Put the reason in the chapter's prose, not in the file. A twin gets the
same number and stem. If the C++ twin has a run-time effect the reader should know about (a crash, a leak), say it in a comment in the file and in the chapter, because the checker only
promises that g++ accepts it.
