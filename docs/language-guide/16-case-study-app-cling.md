# 16. Case study: app-cling

The cJSON chapter tested the safety claims on a library with a published bug list. This chapter looks at a different kind of program: an application, written the way a CeeLing user would write one, with objects, closures, threads and many files. The question is not "how many bugs does it remove" but "what does the language do for a real program, what does it do to it, and where do you see the cost".

**app-cling** (`C:\project\app-cling`) is a framework for desktop applications. A window shows a web page (the system WebView, WebView2 on Windows); the page calls named *commands*, and the commands are CeeLing code: reading a file the program has allowed, running a program from a fixed list, opening a second window, watching a folder, showing a dialog. The page never touches the operating system. It sends `["fs.read", {"path": "notes.txt"}]` and gets a JSON reply or an error. Its own README calls it a prototype, and that is the right way to read the numbers below: this is a program written to test the language, not a product with users.

```
page (JavaScript)  ->  native layer shim/ (C++, trusted)  ->  CeeLing program src/*.clg (checked)
 cling.fs.read(..)       queue, window, OS primitives          dispatcher, file policy, every decision
```

Two layers, and the line between them is the point:

- **`src/`** is CeeLing, checked by the compiler: the dispatcher, the file policy (what path a page may reach), the list of programs it may start, window bookkeeping, settings, menus, dialogs.
- **`shim/`** is hand-written C++, not checked by anything: it opens the window, queues calls, opens files and runs programs, as small primitives taking and returning numbers, strings and vectors of those. The project's rule is that a decision never lives here.

The sizes, measured by the project's own scripts on the code as it stands (the generator compiles the modules, then runs `scripts/ceeling_stats.sh` on the result and `scripts/shim_lines.sh`; it needs the sibling projects `app-cling` and `cJSON-cling`):

<!-- command: 16_app_cling_stats.py -->
```
$ scripts/ceeling_stats.sh <work directory>
CeeLing source (src/*.clg): 6345 lines of code (comments and blank lines left out)
  ext_fn declarations (the trusted native surface): 72 different functions (93 declaration lines: a function used by several files is declared in each)
  closures: 49 (spawn jobs among them: 7)
  manual-memory words in the source (delete, .get(), reinterpret, std::move, unique_ptr, shared_ptr, weak_ptr): 0
Generated C++ from those files (<work directory>):
  std::unique_ptr: 81   std::shared_ptr: 590   std::weak_ptr: 0   std::move: 67
  bounds checks inserted (cling::at): 520
  null checks inserted (cling::nn):   612
$ scripts/shim_lines.sh
win    2409 lines of code (976 shared + 1433 in win/); 2873 physical lines  OVER THE LIMIT of 1000
posix  1363 lines of code (976 shared + 387 in posix/); 1682 physical lines  OVER THE LIMIT of 1000
```

Read it as follows. The program is about 6,300 lines of CeeLing (code only, comments and blank lines left out) in 19 files. Its trusted native layer is 2,409 lines of C++ for Windows (976 shared files plus 1,433 for the OS) and 1,363 for Linux. So on Windows a little over a quarter of the application's code is code the compiler does not check. The script also prints "OVER THE LIMIT of 1000": the project once set that limit, then dropped it as a rule (`app-cling/TODO.md`), and its README still quotes the old figure of 1,059 lines. This chapter uses the measured one.

That is the first honest number about what CeeLing does for a real program: it checks the majority of the application, and it does not check the rest. What the rest can do is bounded by a list of 72 declared functions, below.

## What the program looks like

Nothing in the source looks unusual if you have read Java. Here is a command, `fs.read`, as it is registered. Just above it, `FileGate files = gate;` copies the application's file policy into a local; the closure captures that local and a number, by value:

<!-- source: app-cling/src/app.clg "closure Command [files, maxReadBytes] (Item args)" 12 -->
```
        command("fs.read", closure Command [files, maxReadBytes] (Item args) {
            string path = requiredString(args, "path");
            FsResult r = files.read(path, maxReadBytes);
            fsCheck(r.status, "cannot read " + path);
            if (!isUtf8(r.text)) {
                throw "not_text: the file is not valid UTF-8";
            }
            Item result = createObject();
            addStringToObject(result, "text", r.text);
            return result;
        });
        command("fs.write", closure Command [files] (Item args) {
            ...
```

`Command` is a class with one virtual method, `call`. A `closure Command [captures] (params) { ... }` is an anonymous subclass whose fields are the captured variables ([chapter 9](09-closures.md)). There is no `[&]`: a command cannot keep a reference to something that is gone, because the capture list cannot say so. The `.` on `r` and `files` are checked dereferences, and a missing file is a thrown string, not a null.

**Errors are values with a code.** The only thing CeeLing code can throw is a string. By convention it is `code: text`, and the dispatcher turns it into the page's `{code, message}`:

<!-- source: app-cling/src/core.clg "string errorCodeOf(string thrown) {" -->
```
string errorCodeOf(string thrown) {
    int n = errorPrefixLength(thrown);
    if (n == 0) {
        return "command_failed";
    }
    return thrown.substr(0, n - 2);
}
```

The dispatcher calls the command inside a `try`, catches the string, and answers with `errorJson(errorCodeOf(message), errorTextOf(message))`. A malformed request never stops the application: it becomes an error value.

**A command that acts on "its" window** captures a small shared object (`CallContext who = dispatcher.caller;`, just above it), not a global. `window.open` also reads its arguments into a `struct` that the compiler decodes from the JSON itself ([chapter 3](03-values-and-containers.md) describes structs):

<!-- source: app-cling/src/windows.clg "struct OpenWindowArgs {" -->
```
struct OpenWindowArgs {
public:
    string start;
    string title;
    int width;
    int height;
    bool modal;
    OpenWindowArgs() {
        start = "";
        title = "";
        width = 800;
        height = 600;
        modal = false;
    }
};
```

<!-- source: app-cling/src/app.clg "closure Command [who, book, max] (Item args)" 14 -->
```
        command("window.open", closure Command [who, book, max] (Item args) {
            OpenWindowArgs given = readOpenWindowArgs(args);
            string start = given.start;
            if (!pagePathOk(start)) {
                throw "invalid_argument: `start` must be a path inside the site, such as /settings.html";
            }
            string title = given.title;
            if (title.length() > 0 && !plainText(title, 200)) {
                throw "invalid_argument: `title` must be plain text of at most 200 bytes";
            }
            if (given.width < 200 || given.width > 10000) {
                throw "invalid_argument: `width` must be 200 to 10000";
            }
            if (given.height < 150 || given.height > 10000) {
            ...
```

`decodeOpenWindowArgs` is not written anywhere in the program. A call to `decode<StructName>` makes the compiler write the decoder, which checks the type of each field and names the path of a wrong one. `16_app_decode_args.clg` is the same idea with a stand-in for the page (it needs `cJSON-cling`, as app-cling does):

<!-- example: 16_app_decode_args.clg -->
```
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"

struct OpenArgs {                      // the shape of one command's argument (app-cling's window.open has this shape)
public:
    string title;
    int width;
    bool debug;
    OpenArgs() {                       // no parameters: every field is optional, and this is the default
        title = "untitled";
        width = 800;
        debug = false;
    }
};

string show(OpenArgs a) {
    int d = 0;
    if (a.debug) {
        d = 1;
    }
    return a.title + " " + std::to_string(a.width) + " " + std::to_string(d);
}

int main() {
    std::cout << show(decodeOpenArgs(parse("{}"))) << std::endl;
    std::cout << show(decodeOpenArgs(parse("{\"title\":\"Notes\",\"width\":1024,\"debug\":true}"))) << std::endl;
    try {
        OpenArgs a = decodeOpenArgs(parse("{\"width\":\"wide\"}"));
        std::cout << show(a) << std::endl;
    } catch (string e) {
        std::cout << e << std::endl;
    }
    return 0;
}
```
<!-- output: 16_app_decode_args.clg -->
```
untitled 800 0
Notes 1024 1
invalid_argument: `width` must be a number
```

**A command that takes long** starts a thread. `spawn` takes a closure; what it may capture is plain values only, so the program copies what it needs into locals first, and the worker answers the page itself:

<!-- source: app-cling/src/app.clg "spawn(closure Job [exe, argv, cwd, env, timeoutMs, maxBytes, id, handle] () {" 8 -->
```
            spawn(closure Job [exe, argv, cwd, env, timeoutMs, maxBytes, id, handle] () {
                string out = "";
                string err = "";
                int exitCode = 0;
                int flags = 0;
                int status = app_proc_run(handle, id, exe, argv, cwd, env, timeoutMs, maxBytes, out, err, exitCode, flags);
                finishRun(handle, id, status, out, err, exitCode, flags);
            });
                ...
```

`exe`, `argv`, `cwd`, `env` are strings and vectors of strings, `id` and `handle` identify the page call. A worker cannot hold the file policy, the window book or any other object, because the compiler refuses to let an object into a thread it does not own ([chapter 10](10-threads.md)). That one rule shaped the design: `ToolRun` and `ToolResult` are structs because they travel; the state that belongs to the main thread stays there.

**A worker that needs the main thread** sends a request on a `Channel` and waits for the reply on another. The main loop answers from a *pump*, a closure it runs when a worker wakes it. The product has the mechanism (`Pump`, `onPump`, `wakeMain` in `later.clg` and `app.clg`); the product's own commands do not use a `Channel` yet, and the test program does:

<!-- source: app-cling/tests/headless_main.clg "closure Pump [toMain] (FileGate gate)" 12 -->
```
    app.onPump("files", closure Pump [toMain] (FileGate gate) {
        while (toMain.wait(0)) {
            FileAsk ask = toMain.take();
            FsResult r = gate.read(ask.path, 100000);
            if (r.status == 0) {
                ask.reply.send(r.text);
            } else {
                ask.reply.send("ERR");
            }
        }
    });
    app.asyncCommand("viapump", closure AsyncCommand [toMain] (Item args, string id, int handle) {
        ...
```

`16_app_worker_pump.clg` is that exchange reduced to one file. Everything crossing the thread boundary is a string, a number or a `Channel`; the `Gate` object is used by the main thread only, and nothing is shared except the channels:

<!-- example: 16_app_worker_pump.clg -->
```
class Job { public: Job() { } virtual void run() { } };

class Gate {                       // only the main thread may use it: it never enters a closure
public:
    int reads;
    Gate() { reads = 0; }
    string read(string path) {
        reads = reads + 1;
        return "contents of " + path;
    }
};

struct Request {                   // plain values and a Channel: the one thing a worker may share
public:
    string path;
    Channel<string> reply;
    Request(string p, Channel<string> r) { path = p; reply = r; }
};

int main() {
    Gate gate = new Gate();
    Channel<Request> toMain;
    Channel<string> done;
    spawn(closure Job [toMain, done] () {
        Channel<string> reply;
        toMain.send(new Request("a.txt", reply));
        string text = "";
        if (reply.wait(5000)) {
            text = reply.take();
        }
        done.send("worker got: " + text);
    });
    if (toMain.wait(5000)) {          // in app-cling this loop is a Pump that the main loop runs when a worker wakes it
        Request r = toMain.take();
        r.reply.send(gate.read(r.path));
    }
    if (done.wait(5000)) {
        std::cout << done.take() << std::endl;
    }
    std::cout << "gate reads: " << gate.reads << std::endl;
    return 0;
}
```
<!-- output: 16_app_worker_pump.clg -->
```
worker got: contents of a.txt
gate reads: 1
```

None of this source contains a pointer type, a `delete`, a `std::move`, a `.get()` or a `reinterpret_cast`. The project counts them: the word list `delete`, `.get()`, `reinterpret`, `std::move`, `unique_ptr`, `shared_ptr`, `weak_ptr` matches **0** lines of the program's code. That is not a measure of safety (there are no such words in a Java program either); it is a measure of what the programmer was spared writing. The next section is what the compiler wrote instead.

## The pointers the compiler inferred

The source never says who owns what. For every class field, every container of objects, every method result and every object a closure captures, the compiler chose `unique_ptr` (one owner), `shared_ptr` (several) or `weak_ptr` (looks, does not own). The rules and their purpose are in [chapter 4](04-ownership.md); the point of this section is what they did on a program with 19 files.

The table is produced by `16_app_cling_pointers.py`, which does what `app-cling/scripts/build.sh` does: runs the pointer-demands step ([chapter 12](12-many-files.md)), compiles each module to a header and a `.cpp`, and reads the pointer kinds out of the generated C++. A row is one declaration, or a group of identical ones (the 13 closures that all capture `files` are one row). Modules are in the project's build order, then the four modules of `cJSON-cling` that the program links.

**How to read the "reason" column.** The compiler records why it chose a kind, but it prints almost nothing of it. The comment at the top of a generated `.cpp` that older documents mention exists only in the legacy generator, and it lists the first pass, before the later rules promote things. The one place the final reason is written out is the demands file the build uses (`--ptr-demands-out`), and only for fields of other modules' classes. So each reason starts with a word that says where it comes from:

| Word | Meaning |
|---|---|
| `compiler` | the compiler's own words, taken from a demands file |
| `demand` | unique when the module is compiled alone, shared once the other modules' demands are read |
| `hint` | a `//@ptr` comment in the source |
| `rule` | a rule of the analysis that applies by its definition (a class with a weak field; a virtual method) |
| `pattern` | **the script found a shape in the source** that one of the rules is about (a field copied into a local, an element copied out of a vector). The compiler did not say that this rule fired. Treat it as the likely reason, not a proof |
| `default` | no rule made it shared; unique is where the analysis starts |
| `none` | the compiler prints no reason and the script found no pattern |

<!-- command: 16_app_cling_pointers.py -->
```
| module | class | member | kind | reason |
|---|---|---|---|---|
| core | Command | result of `call()` (Item) | shared_ptr | rule: virtual method with an object result (the compiler makes such signatures shared so a hierarchy agrees) |
| core | (free functions) | results of 2 free functions (Item): `fieldOf`, `objectArg` | shared_ptr | none: not printed, no pattern found |
| files | FileGate | results of 11 methods (FsResult, Opened): `addGrant`, `grantFile`, `grantFolder`, `list`, ... | unique_ptr | default: no rule made it shared |
| settings | SettingsStore | field `gate` (FileGate) | unique_ptr | default: no rule made it shared |
| settings | SettingsStore | result of `allItem()` (Item) | shared_ptr | pattern: returns the local `Item all = createObject()` |
| settings | SettingsStore | result of `valueItem()` (Item) | shared_ptr | pattern: returns the local `Item made = createNumber(numbers[i])` |
| placement | WindowMemory | field `gate` (FileGate) | unique_ptr | default: no rule made it shared |
| dispatch | Dispatcher | elements of `asyncHandlers` (AsyncCommand) | shared_ptr | pattern: dispatch.clg:391 `AsyncCommand starter = asyncHandlers[asyncIndex];` |
| dispatch | Dispatcher | elements of `handlers` (Command) | shared_ptr | pattern: dispatch.clg:415 `Command handler = handlers[index];` |
| dispatch | Dispatcher | field `caller` (CallContext) | shared_ptr | demand: another module needs it shared; compiler: passed to a parameter that is stored into an already-shared field |
| dispatch | Dispatcher | field `rules` (CommandRules) | shared_ptr | demand: another module needs it shared; compiler: field is populated from a stored constructor parameter, matching the SHARED default a fresh inline-constructed argument already gets |
| dispatch | Dispatcher | results of 1 method (Reply): `handle` | unique_ptr | default: no rule made it shared |
| fscommands | (free functions) | results of 1 free function (Item): `entryItem` | shared_ptr | none: not printed, no pattern found |
| programs | ProgramRegistry | elements of `specs` (ProgramSpec) | shared_ptr | pattern: programs.clg:80 `ProgramSpec spec = specs[i];` |
| programs | (free functions) | results of 1 free function (FsResult): `resolveProgram` | unique_ptr | default: no rule made it shared |
| later | LaterBook | elements of `items` (Later) | shared_ptr | pattern: later.clg:44 `items[index] = items[last];` |
| later | PumpBook | elements of `items` (Pump) | shared_ptr | pattern: later.clg:44 `items[index] = items[last];` (in class LaterBook: containers are told apart by name only) |
| watches | WatchState | elements of `pending` (WatchEvent) | shared_ptr | pattern: watches.clg:71 `WatchEvent e = pending[i];` |
| watches | Watches | elements of `states` (WatchState) | shared_ptr | pattern: watches.clg:113 `WatchState s = states[i];` |
| windows | PopupBook | elements of `menus` (Menu) | shared_ptr | pattern: windows.clg:89 `Menu m = menus[i];` |
| windows | Window | field `menu` (Menu) | shared_ptr | pattern: assigned from a variable: windows.clg:246 `menu = bar;` |
| windows | Window | field `rules` (CommandRules) | shared_ptr | compiler: field is populated from a stored constructor parameter, matching the SHARED default a fresh inline-constructed argument already gets |
| windows | Window | field `state` (WindowState) | shared_ptr | demand: another module needs it shared; compiler: copied between a field and another variable, so both are owners |
| windows | Window | field `watches` (Watches) | shared_ptr | demand: another module needs it shared; compiler: copied between a field and another variable, so both are owners |
| windows | WindowBook | elements of `items` (Window) | shared_ptr | pattern: windows.clg:373 `Window w = items[i];` |
| windows | WindowBook | result of `get()` (Window) | shared_ptr | pattern: returns the local `Window w = items[i]` |
| windows | WindowBook | result of `needed()` (Window) | shared_ptr | pattern: returns the local `Window w = get(h)` |
| theme | ThemeState | field `book` (WindowBook) | shared_ptr | pattern: assigned from a variable: theme.clg:61 `book = windows;` |
| theme | ThemeState | result of `stateItem()` (Item) | shared_ptr | pattern: returns the local `Item e = createObject()` |
| drops | DropRules | field `gate` (FileGate) | shared_ptr | pattern: assigned from a variable: drops.clg:34 `gate = files;` |
| app | App | elements of `trayMenus` (Menu) | shared_ptr | pattern: app.clg:1268 `Menu tm = trayMenus[ti];` |
| app | App | field `dispatcher` (Dispatcher) | unique_ptr | default: no rule made it shared |
| app | App | field `drops` (DropRules) | unique_ptr | default: no rule made it shared |
| app | App | field `gate` (FileGate) | shared_ptr | pattern: copied into a local: app.clg:277 `FileGate files = gate;` |
| app | App | field `laters` (LaterBook) | unique_ptr | default: no rule made it shared |
| app | App | field `links` (LinkRules) | shared_ptr | pattern: copied into a local: app.clg:276 `LinkRules rules = links;` |
| app | App | field `memory` (WindowMemory) | shared_ptr | pattern: copied into a local: app.clg:1051 `WindowMemory mem = memory;` |
| app | App | field `menuHandler` (MenuAction) | shared_ptr | pattern: copied into a local: app.clg:1274 `MenuAction trayItem = menuHandler;` |
| app | App | field `notifier` (Notifier) | shared_ptr | pattern: copied into a local: app.clg:1386 `Notifier shared = notifier;` |
| app | App | field `notifyHandler` (NotificationAction) | shared_ptr | pattern: copied into a local: app.clg:1404 `NotificationAction handler = notifyHandler;` |
| app | App | field `popups` (PopupBook) | shared_ptr | pattern: copied into a local: app.clg:1314 `PopupBook book = popups;` |
| app | App | field `programs` (ProgramRegistry) | shared_ptr | pattern: copied into a local: app.clg:583 `ProgramRegistry registry = programs;` |
| app | App | field `pumps` (PumpBook) | unique_ptr | default: no rule made it shared |
| app | App | field `settings` (SettingsStore) | shared_ptr | pattern: copied into a local: app.clg:189 `SettingsStore settings = this.settings;` |
| app | App | field `theme` (ThemeState) | shared_ptr | pattern: copied into a local: app.clg:334 `ThemeState state = theme;` |
| app | App | field `trayHandler` (TrayAction) | shared_ptr | pattern: copied into a local: app.clg:1774 `TrayAction clicked = trayHandler;` |
| app | App | field `watches` (Watches) | shared_ptr | pattern: assigned from a variable: app.clg:1210 `watches = w.watches;` |
| app | App | field `window` (WindowState) | shared_ptr | pattern: assigned from a variable: app.clg:1209 `window = w.state;` |
| app | App | field `windows` (WindowBook) | shared_ptr | pattern: copied into a local: app.clg:202 `WindowBook book = windows;` |
| app | (closures) | captured `book` (PopupBook) in 1 closure: `menu.popup` | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `book` (WindowBook) in 11 closures: `fs.unwatch`, `fs.watch`, `settings.set`, ... | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `caller` (CallContext) in 1 closure: `notification.show` | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `files` (FileGate) in 13 closures: `fs.exists`, `fs.list`, `fs.mkdir`, ... | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `registry` (ProgramRegistry) in 1 closure: `proc.run` | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `rules` (LinkRules) in 3 closures: `shell.openFile`, `shell.openUrl`, `shell.reveal` | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `settings` (SettingsStore) in 3 closures: `settings.get`, `settings.getAll`, `settings.set` | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `shared` (Notifier) in 1 closure: `notification.show` | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `state` (ThemeState) in 2 closures: `theme.get`, `theme.set` | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | captured `who` (CallContext) in 15 closures: `window.close`, `window.focus`, `window.getBounds`, ... | shared_ptr | pattern: a captured object is copied into the closure, and the capturing code keeps its own handle (two owners) |
| app | (closures) | result of `call()` (Item) in 33 closures | shared_ptr | rule: a closure implements a virtual method, and such signatures are shared so a hierarchy agrees |
| cjson_item | Item | field `child` (Item) | shared_ptr | compiler: passed to a read-only free-function parameter (must survive the call); passed to a read-only method parameter (must survive the call) |
| cjson_item | Item | field `next` (Item) | shared_ptr | compiler: bidirectional ownership needs a shared forward edge; copied between a field and another variable, so both are owners |
| cjson_item | Item | field `prev` (Item) | weak_ptr | hint: `//@ptr weak Item.prev` at line 13 |
| cjson_item | (free functions) | results of 36 free functions (Item): `addArrayToObject`, `addBoolToObject`, `addFalseToObject`, ... | shared_ptr | none: not printed, no pattern found |
| cjson_parse | ParseResult | field `item` (Item) | shared_ptr | rule: class Item has a weak field (Item.prev), so every handle to an instance of Item is shared (compiler: "instances of this class participate in weak back-references") |
| cjson_parse | (free functions) | results of 3 free functions (ParseResult): `parseBuffer`, `parseWithLengthOptsResult`, `parseWithOptsResult` | unique_ptr | default: no rule made it shared |
| cjson_parse | (free functions) | results of 4 free functions (Item): `parse`, `parseWithLength`, `parseWithLengthOpts`, ... | shared_ptr | none: not printed, no pattern found |
| cjson_print | (free functions) | results of 3 free functions (Printed): `printBuffered`, `printPreallocated`, `printToString` | unique_ptr | default: no rule made it shared |

Members that are unique when their module is compiled alone and shared once the demands of the other modules are read: 4 (Dispatcher.caller, Dispatcher.rules, Window.state, Window.watches).

Declarations behind the table (a row for methods, free functions or captures can stand for several):
                                                  unique shared   weak
  app-cling    fields and container elements           6     32      0
  app-cling    results of methods                     12      6      0
  app-cling    results of free functions               1      3      0
  app-cling    objects captured by closures            0     51      0
  app-cling    results of closure methods              0     33      0
  app-cling    all declarations                       19    125      0
  cjson-cling  fields and container elements           0      3      1
  cjson-cling  results of methods                      0      0      0
  cjson-cling  results of free functions               6     40      0
  cjson-cling  objects captured by closures            0      0      0
  cjson-cling  results of closure methods              0      0      0
  cjson-cling  all declarations                        6     43      1

app-cling declares 36 classes and 8 structs. A struct is stored by value, so it has no pointer kind to choose and no row in the table.

Every pointer token in the generated .cpp files of the 19 app-cling modules (the count of scripts/ceeling_stats.sh), by where it stands:
                                                   unique shared   weak
  field or container element in a class body            6     32      0
  captured object in a closure class                    0     51      0
  result type (declaration or definition)              25     81      0
  parameter type (declaration or definition)            0    266      0
  inside a function body                               50    160      0
  all                                                  81    590      0
Parameters are not listed in the table: a parameter of class type follows the kind of its class where it is declared.

How the two counts meet (app-cling modules only; each class is written into its own .cpp as well as its .h, and a method result appears in the class and in the definition):
  unique  fields, elements and captures: table   6, .cpp   6   results: 2 x (methods + closure methods) + free functions =  25, .cpp  25   agree
  shared  fields, elements and captures: table  83, .cpp  83   results: 2 x (methods + closure methods) + free functions =  81, .cpp  81   agree
  weak    fields, elements and captures: table   0, .cpp   0   results: 2 x (methods + closure methods) + free functions =   0, .cpp   0   agree
```

### How the table agrees with the project's count

The project's `ceeling_stats.sh` reports `std::unique_ptr: 81, std::shared_ptr: 590, std::weak_ptr: 0`. The table counts *declarations* (144 for the application's 19 files: 19 unique, 125 shared, none weak), not occurrences. The last block of the output closes the gap exactly. A pointer type is written in a class body (the fields, container elements and captured objects: 38 + 51), as a method result (declared in the class and written again in the definition), as a parameter (266, all `shared_ptr`, not listed because a parameter follows the kind of its class), and inside function bodies (210: locals, `make_shared`, casts). Fields, elements, captures and results add up to the same totals as the `.cpp` files, kind by kind.

### What the table shows

**Almost everything is shared, for two reasons that come from how the program is built.** 125 of 144 declarations are `shared_ptr`. Two patterns account for most of them:

1. **Closures capture objects.** A closure stores a copy of the handle and the code that registered it keeps its own. That is two owners of one object, so the captured object is `shared_ptr` whatever else is true of it. The 51 captures in the table are all shared: `files` (13 closures), `who` (15), `book` (12), and so on. A small experiment, not in the table, shows how little it takes: in a program with one closure that captures a counter, the counter is shared even if the registering code never uses it again. The consequence goes back into the fields. `App` writes `FileGate files = gate;` to capture the file policy, copying a field into a local, and that copy is what makes `App.gate` shared as well (the `pattern` rows in the `app` block).
2. **Books of objects looked up by handle.** `WindowBook`, `Dispatcher`, `ProgramRegistry`, `Watches` and others keep a vector of objects and hand one out (`Window w = items[i]; return w;`). A `unique_ptr` element cannot be copied out, so the elements are shared. Ten container rows are this. `16_app_book_elements.clg` has both sides: a book that hands its elements out, and a journal that only reads them in place:

<!-- example: 16_app_book_elements.clg -->
```
class Window {
public:
    int handle;
    string title;
    Window(int h, string t) { handle = h; title = t; }
};

class WindowBook {                         // like app-cling's WindowBook: a vector of objects and a lookup
public:
    std::vector<Window> items;
    WindowBook() { }
    void add(Window w) { items.push_back(w); }
    int indexOf(int handle) {
        int i = 0;
        while (i < items.size()) {
            if (items[i].handle == handle) {
                return i;
            }
            i = i + 1;
        }
        return -1;
    }
    Window get(int handle) {
        int i = indexOf(handle);
        if (i < 0) {
            return nullptr;
        }
        Window w = items[i];               // the book and the caller now both hold this window
        return w;
    }
};

class Entry {
public:
    string text;
    Entry(string t) { text = t; }
};

class Journal {                            // elements are only added and read in place: nobody else holds them
public:
    std::vector<Entry> entries;
    Journal() { }
    void note(string text) { entries.push_back(new Entry(text)); }
    string last() { return entries[entries.size() - 1].text; }
};

int main() {
    WindowBook book = new WindowBook();
    book.add(new Window(7, "Notes"));
    Window w = book.get(7);
    std::cout << "window " << w.handle << ": " << w.title << std::endl;
    Journal journal = new Journal();
    journal.note("opened 7");
    journal.note("closed 7");
    std::cout << journal.entries.size() << " entries, last: " << journal.last() << std::endl;
    return 0;
}
```
<!-- output: 16_app_book_elements.clg -->
```
window 7: Notes
2 entries, last: closed 7
```
<!-- generated: 16_app_book_elements.clg "class WindowBook {" -->
```
class WindowBook {
public:
    std::vector<std::shared_ptr<Window>> items;
    WindowBook();
    void add(std::shared_ptr<Window> w);
    int indexOf(int handle);
    std::shared_ptr<Window> get(int handle);
};
```
<!-- generated: 16_app_book_elements.clg "class Journal {" -->
```
class Journal {
public:
    std::vector<std::unique_ptr<Entry>> entries;
    Journal();
    void note(const std::string& text);
    std::string last();
};
```

The book's elements and its `get` result are `shared_ptr`; the journal's elements are `unique_ptr`, because `entries[i].text` is read where it stands. The difference is one line in the source and no annotation.

**The command table is the typical case.** `16_app_command_table.clg` is app-cling's `Dispatcher` and a few commands in miniature:

<!-- example: 16_app_command_table.clg -->
```
class Counter {
public:
    int n;
    Counter() { n = 0; }
};

class Command {
public:
    Command() { }
    virtual int call(int a, int b) { return 0; }
};

class Dispatcher {
public:
    std::vector<string> names;
    std::vector<Command> handlers;
    Dispatcher() { }
    void add(string name, Command handler) {
        names.push_back(name);
        handlers.push_back(handler);
    }
    string handle(string name, int a, int b) {
        int i = 0;
        while (i < names.size()) {
            if (names[i] == name) {
                Command handler = handlers[i];
                return name + " " + std::to_string(handler.call(a, b));
            }
            i = i + 1;
        }
        return name + ": unknown command";
    }
};

int main() {
    Dispatcher dispatcher = new Dispatcher();
    Counter hits = new Counter();
    dispatcher.add("add", closure Command [] (int a, int b) { return a + b; });
    dispatcher.add("hits", closure Command [hits] (int a, int b) {
        hits.n = hits.n + 1;
        return hits.n;
    });
    std::cout << dispatcher.handle("add", 2, 3) << std::endl;
    std::cout << dispatcher.handle("hits", 0, 0) << std::endl;
    std::cout << dispatcher.handle("hits", 0, 0) << std::endl;
    std::cout << dispatcher.handle("nope", 0, 0) << std::endl;
    return 0;
}
```
<!-- output: 16_app_command_table.clg -->
```
add 5
hits 1
hits 2
nope: unknown command
```
<!-- generated: 16_app_command_table.clg "class Dispatcher {" -->
```
class Dispatcher {
public:
    std::vector<std::string> names;
    std::vector<std::shared_ptr<Command>> handlers;
    Dispatcher();
    void add(const std::string& name, std::shared_ptr<Command> handler);
    std::string handle(const std::string& name, int a, int b);
};
```
<!-- generated: 16_app_command_table.clg "class __closure_16_app_command_table_1" -->
```
class __closure_16_app_command_table_1;

#ifndef CEELING_DECL_COUNTER
#define CEELING_DECL_COUNTER
class Counter {
public:
    int n{};
    Counter();
};
```
<!-- generated: 16_app_command_table.clg "int main()" -->
```
int main() {
    std::unique_ptr<Dispatcher> dispatcher = std::make_unique<Dispatcher>();
    std::shared_ptr<Counter> hits = std::make_shared<Counter>();
    cling::nn(dispatcher, "line 38")->add("add", std::make_shared<__closure_16_app_command_table_0>());
    cling::nn(dispatcher, "line 39")->add("hits", std::make_shared<__closure_16_app_command_table_1>(hits));
    std::cout << cling::nn(dispatcher, "line 43")->handle("add", 2, 3) << std::endl;
    std::cout << cling::nn(dispatcher, "line 44")->handle("hits", 0, 0) << std::endl;
    std::cout << cling::nn(dispatcher, "line 45")->handle("hits", 0, 0) << std::endl;
    std::cout << cling::nn(dispatcher, "line 46")->handle("nope", 0, 0) << std::endl;
    return 0;
}
```

The vector of commands is a vector of `shared_ptr` because the dispatcher copies one out to call it. The captured `Counter` is shared because the closure and `main` both hold it. The `Dispatcher` itself, held by one variable, is `unique_ptr`. The same three facts are in the real table (`Dispatcher.handlers`, the captured objects, `App.dispatcher`).

**A few are unique, and for the plain reason that one place owns them.** `App.dispatcher`, `laters`, `pumps` and `drops` have one owner. So do `SettingsStore.gate` and `WindowMemory.gate`: each of those classes makes its *own* `FileGate` (`gate = new FileGate();`), because the settings file must not be reachable through the page's gate. The same field name in `DropRules` is shared, because that class is given the application's gate (`gate = files;`) and so is `App.gate`. One name, four classes, two kinds, and the difference is visible in the source: who made the object. The results of the 11 `FileGate` methods and of `Dispatcher.handle` are unique because each returns a fresh object to a single caller.

**Only one `weak_ptr` exists in the whole program, and it is not in app-cling.** It is `Item.prev` in the JSON library, which carries an explicit `//@ptr weak Item.prev` hint. Because `Item` has a weak field, the compiler makes every handle to an `Item` shared (the rule's own words are in the table), which is why every free function that returns a JSON tree is shared. `16_app_weak_prev.clg` shows the shape in a few lines; it uses the naming rule (a field called `prev` next to `next`) instead of a hint:

<!-- example: 16_app_weak_prev.clg -->
```
class Item {
public:
    int value;
    Item next;                     // owns the rest of the list
    Item prev;                     // looks back: the name and the shape (next points forward, prev back) tell the compiler it must not own
    Item(int v) { value = v; }
};

class List {                       // the shape of cjson-cling's Item: next and child own, prev is weak
public:
    Item head;
    Item tail;
    List() { }
    void append(int v) {
        Item node = new Item(v);
        if (head == nullptr) {
            head = node;
            tail = node;
            return;
        }
        tail.next = node;
        node.prev = tail;
        tail = node;
    }
};

int main() {
    List list = new List();
    list.append(1);
    list.append(2);
    list.append(3);
    string forward = "";
    Item cur = list.head;
    while (cur != nullptr) {
        forward = forward + std::to_string(cur.value) + " ";
        cur = cur.next;
    }
    std::cout << forward << std::endl;
    string backward = "";
    cur = list.tail;
    while (cur != nullptr) {
        backward = backward + std::to_string(cur.value) + " ";
        cur = cur.prev;
    }
    std::cout << "back: " << backward << std::endl;
    Item first = list.head;
    if (first.prev == nullptr) {
        std::cout << "first has no prev: " << first.value << std::endl;
    }
    return 0;
}
```
<!-- output: 16_app_weak_prev.clg -->
```
1 2 3
back: 3 2 1
first has no prev: 1
```
<!-- generated: 16_app_weak_prev.clg "class Item {" -->
```
class Item {
public:
    int value{};
    std::shared_ptr<Item> next;
    std::weak_ptr<Item> prev;
    Item(int v);
    ~Item();
};
```

app-cling's own classes have no back pointer, by design. The one reference cycle that did exist (the dispatcher owned its commands and the window commands captured the dispatcher, so it leaked at exit) was found by LeakSanitizer, not by the compiler, and removed by moving the shared state into two small objects, `CommandRules` and `CallContext`, that the commands hold instead of the dispatcher:

<!-- source: app-cling/src/dispatch.clg "CommandRules rules;" -->
```
    CommandRules rules;
    CallContext caller;

    Dispatcher() {
        defaultTimeoutMs = 0;
        rules = new CommandRules();
        caller = new CallContext();
    }
```

CeeLing does not detect cycles ([chapter 13](13-contract-and-threats.md), row 12). A `shared_ptr` cycle is a leak, not a dangling reference; the guarantee holds and the program is still wrong. The inference chose `shared_ptr` for `Dispatcher.rules` and `Dispatcher.caller` only after another module asked for it (the `demand` rows).

**What the table says about how the inference behaves on a real program.**

- *It chooses shared whenever it cannot prove one owner.* That is the rule "ambiguous inference reads as shared, never as guessing unique", and on this program it means 87% of declarations. A programmer who wanted fewer atomic reference counts would restructure (keep elements in place; hand out a `ref` observer) and could see the effect in this table.
- *It works by name in places.* The analysis tells container fields apart by name. `PumpBook.items` is shared only because `LaterBook.items`, in the same file, copies an element (`items[index] = items[last];`). A scratch copy of `later.clg` with `PumpBook.items` renamed `pumpList` makes its elements `unique_ptr`. Nothing breaks either way; it costs a little speed and is the kind of thing a table like this makes visible.
- *The reasons are mostly not printed.* Of the rows that are shared, the compiler's own words cover the four `demand` rows, three more fields, and the weak-field rule on `Item`. For the rest this chapter's script has read the source. An improvement the compiler could make is to print the final reason for every member, as it does for the ones other modules reach.
- *Pointer kinds are decided per file, and a header fixes them.* That is why the table has `demand` rows at all: see "One program, many files" below.

## Closures and threads, counted

`ceeling_stats.sh` counts 49 closures in the program. By the interface they implement: 33 `Command`, 8 `AsyncCommand`, 7 `Job` (the `spawn` bodies) and 1 `Failure` (the handler that rejects a page's promise when a worker throws). Each one lists what it captures, each capture is by value, and the compiler turned each into a class (the 51 captured objects and 33 result types in the table are those classes). Seven `spawn` calls in `app.clg` are the program's only threads of its own. Every one captures strings, numbers and vectors of those; the compiler would refuse a class object.

The one `Failure` closure is how a worker that throws still answers the page. It captures the call id and the window handle, both plain values:

<!-- source: app-cling/src/dispatch.clg "onJobFailure(closure Failure [id, appHandle] (string message) {" 4 -->
```
                onJobFailure(closure Failure [id, appHandle] (string message) {
                    replyError(appHandle, id, errorCodeOf(message), errorTextOf(message));
                });
                starter.start(args, id, appHandle);
                    ...
```

What this does and does not give you is the threat model of [chapter 10](10-threads.md): in checked CeeLing code a spawned job reaches only its own copies, objects moved in, and `Channel` state, so there are no data races through `spawn` or `Channel`. It says nothing about deadlock, about a worker calling a native function that touches shared state, or about the order in which things happen. The project found one of those (below).

## The run-time checks, counted and priced

The compiler inserted **520** checked subscripts (`cling::at`, bounds) and **612** checked dereferences (`cling::nn`, null), and 67 `std::move` calls where ownership passes. app-cling is built with both checks on. A check names the `.clg` line and stops the program; it is `abort()`, not an exception ([chapter 7](07-run-time-checks.md)).

What a failed check looks like, in a command that trusts a number from the page (`16_app_index_abort.clg`; the page sent 3 and the list has 2 entries). The program stops at the line, with the index and the size; the first call, which was in range, had already answered:

<!-- example: 16_app_index_abort.clg -->
```
// A command that trusts a number from the page. The page sent 3, the list has 2 entries.
class Command {
public:
    Command() { }
    virtual string call(int index) { return ""; }
};

int main() {
    std::vector<string> names;
    names.push_back("notes.txt");
    names.push_back("todo.txt");
    Command pick = closure Command [names] (int index) {
        return names[index];
    };
    std::cout << pick.call(1) << std::endl;
    std::cout << pick.call(3) << std::endl;
    return 0;
}
```
<!-- output: 16_app_index_abort.clg -->
```
todo.txt
```

The project measures the cost with `scripts/bench_checks.sh`, which builds the same program twice (checks on, `--no-bounds-checks --no-null-checks`) and times three operations through the real dispatcher and file policy, with the window replaced by test doubles. The project's document gives one run (one Windows machine, MinGW g++, `-O1`, best of five rounds); the second column is a run of the same script made while this chapter was written:

| operation | the project's document | a run made for this chapter |
|---|---|---|
| `add`, a command that does almost nothing | +3.8% | +5.8% |
| `fs.read` of a 4 KiB file | +0.6% | +0.7% |
| `fs.list` of a folder with 200 files | +0.4% | +1.2% |

The project says repeated runs differ by 2 to 3 percent, and these two columns agree with that. Read it as "a few percent at most on this workload", not as three precise figures. It leaves out what a real call costs through WebView2 and the browser, which is milliseconds, so the share of the checks is smaller in use. It would not hold for code that loops over large vectors with a subscript: on zlib-cling, CeeLing's own measurement is +5.5% to decompress and +11.3% to compress ([chapter 7](07-run-time-checks.md)). These figures come from the project's script; the guide's checker does not repeat them.

## One program, many files

The program is 19 files, not one. (The project's own document says 17; it was written before two more were added. Chapter and program use what the code says.) The application began as one file of 3,915 lines and was split into modules, each compiled to its own `.cpp` and header and linked, so that a change to one file recompiles only that file.

The list of modules, in the order each comes after the ones it includes, is one line of the project (`scripts/modules.sh`):

<!-- source: app-cling/scripts/modules.sh "APP_MODULES=" 1 -->
```
APP_MODULES="text core files assets settings links placement menu dispatch fscommands dialogs notify programs later watches windows theme drops app"
    ...
```

Splitting broke the inference, in a way worth knowing about. Pointer kinds are chosen for one file at a time, and a header fixes the kind of every field, container element and method result. A module that copies an element out of another module's vector needs `shared_ptr` where the declaring module, which cannot see that use, wrote `unique_ptr`; the two generated files then do not fit and `g++` stops with a conversion error. The answer is **pointer demands**: each module is compiled once to write what it needs from the classes it includes, the demands of all modules are merged, and every module is compiled again reading them ([chapter 12](12-many-files.md) has the mechanism). The generator ran that step before it made the table. Four members changed because of it: `Dispatcher.rules`, `Dispatcher.caller`, `Window.state` and `Window.watches` are `unique_ptr` when their module is compiled alone and `shared_ptr` once the demands are read. They are the `demand` rows above, with the compiler's reason for each.

Was the inference kept by the split? The project compared the table of every pointer kind in the generated C++ of the last single-file commit with the first split one, using its `scripts/ptr_table.py` and the same compiler. This chapter repeated that comparison with the current compiler: the two tables are identical except for one line, in `Watches::flush`, where the single file copies a string field into a temporary before passing it on and the split program does not, and the four new reader functions (`readDialogArgs`, `readMessageArgs`, `readNotifyArgs`, `readOpenWindowArgs`). The project's document gives the cause: the analysis decides whether a callee may modify a string field by matching calls **by name**, so in the large file a name shared with another function made it cautious. One more difference the document does not list: the closures that decode their arguments now call the readers instead of the decoders directly. The comparison shows that the compiler wrote the same pointers. It says nothing about whether the program means the same thing; the project's headless tests (295 checks in the main program, more in the others) do that.

## What is trusted

Everything the compiler does not see is the native layer and what it calls. The checked code reaches the operating system only through `ext_fn` declarations: signatures with no body, taking and returning numbers, strings and vectors of those ([chapter 11](11-talking-to-cpp.md)). There are 72 different functions (93 declaration lines, because a function used by several files is declared in each). A wrong declaration is silently trusted: if the C++ behind `app_fs_read` does something its signature does not say, nothing in CeeLing can notice.

Here are two of the 72, as `files.clg` declares them. The `ref string out` is the one parameter form `ext_fn` allows, an out-parameter; the compiler takes the rest on trust:

<!-- source: app-cling/src/files.clg "ext_fn int app_fs_read(int h, int maxBytes, ref string out);" 1 -->
```
ext_fn int app_fs_read(int h, int maxBytes, ref string out);
    ...
```

What the project does about that is design, not proof:

- policy lives on the checked side. The file policy (`FileGate`) never decides on the *text* of a path; it opens first through a native primitive, asks the operating system where the open handle really points, and compares that with the allowed folders. The C++ opens and reads; it does not know which paths are allowed.
- a request from the page is treated as untrusted input: a command reads its arguments through helpers that reject a wrong shape, because dereferencing a missing field would abort the whole application (the run-time check makes that a clean abort, but a web page should not be able to cause it);
- the project's tests include about 25 *negative controls* that remove one policy check at a time and require the tests to fail, so that the checks are known to be able to fail.

The trusted code, the browser engine, the operating system and the programs `proc.run` starts are all outside the claim. The claim is about the checked code: no dangling references, no use after move, no out-of-bounds subscript or empty-handle dereference that goes unnoticed (it aborts), and no data race through `spawn` or `Channel`. It is not "memory-safe" and not "thread-safe" as a whole program.

## What the tools found, and where the language got in the way

The project's document `docs/ceeling-in-app-cling.md` lists these; they are the other half of the numbers above.

- **A reference cycle the compiler did not see** (above). Found by a leak checker at exit. The fix was a design change.
- **A compiler bug.** A method parameter with the same name as a field was emitted as the field, so `choose(vector names)` in a class with a field `names` assigned the field. It compiled and ran with the wrong data. It is fixed (`tests/safety/param_shadows_field_accepted.clg`). Static checking does not catch a bug in the compiler itself.
- **A race the language does not model.** `fs.watch` answered before the operating system was watching, so a file changed right away could be lost. It is not a memory error, nothing in CeeLing could have caught it, and the project's tests found it under load.

Where the language was in the way, from the same document, each of which cost real code:

- no method call on a call's result (`f(x).length()`), so a local variable first;
- an object returned from a vector element needs `Window w = items[i]; return w;`, and an object copied out of a field gets a different pointer kind than the field;
- an object passed to a free function is moved, so a helper that keeps using it has to be a method;
- a closure inside a closure cannot capture the outer closure's captures (copy them to locals first);
- `std::map` holds only primitives, so tables of objects are parallel vectors (`WindowBook`, `LaterBook`, `PumpBook`);
- generics are fixed: `std::vector`, `std::map`, `Channel<T>`, no user templates.

## What the program does not tell you

- It is one program, written by the people who wrote the compiler, to test the compiler. That is a reason to trust the account of what went wrong (it is in the project's notes) and a reason to doubt that an outside team would have found the same things easy.
- The pointer table says what the compiler chose, not that the choice was needed. The `pattern` rows are the script's reading.
- The numbers are of this program on this compiler. A new rule in the analysis changes the table, and this is why the chapter prints it from a command instead of copying it.
- Nobody has measured the cost of 590 `shared_ptr` against 81 `unique_ptr` here. The project builds with `--pool-shared`, an allocator for shared objects, and measured the checks, not the pointers.

The numbers in `docs/ceeling-in-app-cling.md` were measured at an earlier commit (6,115 lines, 79 `unique_ptr`, 559 `shared_ptr`, 17 modules; the same page says 47 closures in a table and 32 in the text, and 69 and 48 `ext_fn` functions). The figures in this chapter come from running the scripts on the code at hand.
