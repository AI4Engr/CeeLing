# 15. Case study: cJSON's 33 vulnerabilities

The earlier chapters argue that CeeLing removes classes of memory bugs. This one tests the argument on a real library with an independently published bug list: **cJSON 1.7.19**, a widely
vendored C JSON parser, and the 33 issues Joshua Rogers published against it in 2026 (*33 Vulnerabilities in cJSON*).

The method has three steps. Port the whole library to CeeLing (`cJSON-cling`, a sibling project kept next to this repository). Show that the port behaves like the original. Then take each of the 33
issues and show three things: **how cJSON does it** (the defective C code, quoted from the real source, and why it fails), **how cJSON-cling does it** (the port's code for the same job, and the mechanism that
makes the bug impossible, stops it at run time, or leaves it where it was), and **the verdict**. The result is a table of outcomes, not a slogan, and the interesting part is where the answer is "nothing".

> **A caveat that matters.** The article was not available while this chapter was written. Each defect is described here from the one-line descriptions in the notes of the earlier version of this
> chapter, checked against upstream's code. The triggers are *reconstructions*: the smallest program that does what the description says, not the author's proofs of concept. A reconstruction can miss
> the exact path an original exploit takes. For every issue the text says whether the C behaviour was reproduced here, and how. Each "cannot happen" below therefore rests on a *mechanism*
> (what the language no longer lets you write), not only on a passing test.

## How to read an issue

Every issue has a heading with its number and the function it is about, and then:

* **In cJSON.** The code, quoted by the guide's checker from `cJSON-cling/upstream/` (cJSON 1.7.19, commit `c859b25`, the git submodule `upstream/`), and what is wrong with it. Where the defect is
  deterministic there is a **C twin**: a small C program, built with `gcc` against that same source, that shows it. A twin that depends on undefined behaviour says so, and where a defect could not be
  reproduced the text says that instead.
* **In cJSON-cling.** The port's code for the same job, the mechanism, and a CeeLing program built against the port, with what it printed.
* **Verdict.** One of: *cannot happen* (the unsafe operation cannot be written), *stopped at run time* (the program aborts and names the `.clg` line), *partly*, *not prevented*, *avoided by the
  library's own discipline* (not by the language), or *same bug* (a logic error: memory-safe in C too, and reproduced exactly).

All of the CeeLing programs in this chapter that say `#include "cjson_item.clh"` are built with the port's own `tests/build.sh`, so they run against the real library. The outputs shown are what they printed
when the guide was last checked.

## The result

| Outcome in the port | Issues | Count |
|---|---|---|
| **Cannot happen**: the unsafe operation cannot be written | 1, 2, 3, 4, 7, 12, 13, 30, 32 | 9 |
| **Stopped at run time**: aborts with the source line instead of corrupting memory | 5, 11 | 2 |
| **Partly**: one route closed, another still open | 6, 8, 9 | 3 |
| **Not prevented**: CPU exhaustion | 10 | 1 |
| **Avoided by the library's own discipline, not by the language** | 31 | 1 |
| **Same bug, same behaviour**: a logic error, memory-safe in C too | 14 to 29, 33 | 17 |
| | | **33** |

Read it in three parts.

* **Issues 1 to 13, memory errors and crashes.** 9 are cut off (7 cannot happen, 2 are caught at run time), 3 are partly open, 1 is not prevented. Three of the nine carry an asterisk that
  the sections below spell out (4: the cycle is refused by one line of library code; 12: the crash is gone but a malformed node silently reads as an empty path; 13: the failure that is gone is the unchecked
  write, not the out-of-memory condition).
* **Issues 30 to 32, undefined behaviour and shared state.** The language removes two (30, 32). The third (31, `NaN` cast to `int`) the port avoids the way a careful C author would.
* **Issues 14 to 29 and 33, logic errors.** All 17 are reproduced faithfully. A language that checks memory cannot know that `~1` should decode to `/`.

That is the honest shape: CeeLing removes the bugs that come from manual memory management, converts a few into a located abort, and does nothing for bugs in what the program means.

## What was built, and how the port is shown to behave like the original

`cJSON-cling` is a port of `cJSON.c` and `cJSON_Utils.c` (JSON Pointer, Patch, Merge Patch, sorting): 2,907 lines of `.clg` in `src/`, plus a 52-line hand-written C++ shim (`shim/cjson_num.h`)
for number to text (`printf("%g")`) and text to number (`strtod`). The shim is the library's only trusted code. The real data structure is kept: a node has `next`, `prev` and `child`, and the first child's `prev` points
at the last child. What C expresses with a convention, CeeLing expresses in the node's type:

<!-- source: cjson-cling/src/cjson_item.clg "class Item {" -->
```cpp
class Item {
public:
    Item next;
    //@ptr weak Item.prev
    Item prev;
    Item child;
    // Widest first: interleaving small fields between the large ones cost 8 bytes of padding per node.
    string valuestring;
    string key;
    double valuedouble;
    int type;
    int valueint;
    bool isReference;
    bool stringIsConst;
    bool hasValuestring;
    bool hasKey;

    Item() {
        next = nullptr;
        prev = nullptr;
        child = nullptr;
        type = Invalid;
        isReference = false;
        stringIsConst = false;
        hasValuestring = false;
        valueint = 0;
        valuedouble = 0.0;
        hasKey = false;
    }
};
```

`next` and `child` own; `prev` is the one hint the port needs (`//@ptr weak`), because a back link is exactly what the compiler cannot guess from names alone. The port's API differs from the C one where C
returns a pointer into someone else's memory (`cJSON-cling/docs/api-differences.md` lists every difference).

Equivalence is measured, not assumed. These figures are as the project's own documents state them (`cJSON-cling/docs/testing.md`, `docs/performance.md`, the scripts in `cJSON-cling/tests/`); I re-ran the unit tests (776 checks) and the 33-issue matrix on 2026-11-08, and did not re-run the rest.

* **Differential test, core.** The port and upstream parse the same bytes (upstream's own inputs, the 115 files of `tests/diff/corpus`, and 1.5 million mutations); success, error position, end offset and every
  printed byte must match. The internal notes: no divergence.
* **Differential test, Utils.** Pointer lookup, sorting, patch generation and application, merge patches, against the RFC 6902 suites shipped with upstream: about 620,000 comparisons, no divergence (the internal notes).
* **Upstream's own unit tests**, ported to CeeLing programs: 18 programs, 776 checks, all pass (the internal notes counted 20 programs; the folder holds 18, and the 776 was re-run on 2026-11-08; skipped cases are marked in the files).
* **Under sanitizers** (AddressSanitizer and UBSan, in WSL, with the pooled allocator off so a use-after-free is visible): the 776 unit checks, 756 thousand differential parses, 350 thousand Utils comparisons
  and the 20 memory-class triggers give no report except the two stack overflows that are documented below as still open (06b and 09b) (the internal notes). Upstream's own C is not sanitized in that run; the claim
  under test is about the port.

A sanitizer run shows no error on the inputs it saw. It is evidence, not proof, and the fuzzer is mutation-based: it never builds a cyclic tree by hand.

## Why cJSON has these bugs

The list looks long, but it comes from five causes.

1. **Lifetime is managed by hand, with flags.** A node owns its children and strings unless a flag (`cJSON_IsReference`, `cJSON_StringIsConst`) says otherwise, and every function re-implements that
   convention. Issues 1 to 4 and 7 are places where it was applied wrongly.
2. **A pointer can be `NULL` and C does not say so.** Some paths dereference without checking (5, 11, 12) and some write to the result of `malloc` without checking it (13).
3. **Recursive algorithms on input that is untrusted.** Parse, print, compare, duplicate and delete recurse once per nesting level, and nothing bounds the depth of a tree built through the API or by
   a patch that copies a subtree into itself (6, 8, 9). One function recurses twice per level (10).
4. **The code does what it says, and what it says is wrong.** Issues 14 to 29 and 33 are specification mistakes, not memory errors.
5. **Undefined behaviour and shared global state.** Comparing pointers from different allocations (30), casting `NaN` to `int` (31), a `static` error record shared by every caller (32).

CeeLing addresses causes 1, 2 and 5 directly, helps with cause 3 in one place, and is silent on cause 4.

## How the C twins are built

Several C defects are undefined behaviour: a read of freed memory gives whatever the allocator leaves there. To make those deterministic, a twin that needs it installs a small allocator through
cJSON's own `cJSON_InitHooks` (it is part of cJSON's public API, so the library is unmodified): one that "frees" a block by filling it with `0xDD` and keeping it (what a debug heap does, so a later read
finds a known byte pattern), one that fails after N allocations, one that counts live blocks. Each twin carries its own few lines, and says so. Issue 1 is the first and shows the poisoning allocator in full.
It is test scaffolding for the twins and is not part of the port.

The twins that rely on undefined behaviour (1, 2, 3, 4, 31) say so. A twin that dies does so by an access violation or by exhausting the stack, and the checker only requires that the process dies
abnormally, not how.

## Part 1. Lifetime bugs: issues 1, 2, 3, 4, 7

All five share one root: someone calls `free` while someone else still holds a pointer. In CeeLing there is no `free`, no `delete`, no raw pointer to dangle and no `.get()` to extract one
([chapter 8](08-sealed-escape-hatches.md)), so the sentence cannot be written. Ownership is inferred ([chapter 4](04-ownership.md)): a node reachable from two places is shared and lives as long as the last
handle does. The port's `deleteItem` exists so that a call keeps its meaning. In the port `Item` is a shared handle (a node is reachable from its parent and from the caller), so `deleteItem(x)` drops only the callee's copy: the tree lives until the last
handle goes, and a later use of `x` compiles and is safe, because nothing was freed under it (I checked this by compiling it). Where a handle has exactly one owner, handing it over is a move ([chapter 5](05-moves.md)) and the same
use afterwards is a compile-time error. This miniature has one owner, so it shows the rule, not the port:

<!-- example: 15_cjson_01_delete_then_use.clg -->
```cpp
// In miniature, how the port's deleteItem is written: it takes the item and drops it. Using the variable
// afterwards is the use-after-free that cJSON_Delete allows, and here it is a compile-time error.
class Item {
public:
    int valueint;
    Item(int v) { valueint = v; }
};

void deleteItem(Item item) {
}

int main() {
    Item item = new Item(7);
    deleteItem(item);
    std::cout << item.valueint << std::endl;
    return 0;
}
```
<!-- error: 15_cjson_01_delete_then_use.clg -->
```
error[semantic/use-after-move]: use of moved-from value `item`
  --> 15_cjson_01_delete_then_use.clg:15:18
  help: `item` was moved earlier; assign a new value before using it
```

**What did the work in this part.** Honestly: the absence of manual memory management plus the ordinary C++ smart pointers underneath. The dangling-reference analysis of
[chapter 6](06-dangling-references.md) played no part, because the library never uses a `ref` observer. The credit belongs to the sealed hatches and the ownership inference, and to the fact that the
port had to be written without the tools that make these bugs possible.

### Issue 1: `overwrite_item` frees memory it does not own

**Defect.** `overwrite_item` (used by the JSON Patch root-level `add`, `replace` and `remove`) frees the node's child, key and string without looking at the reference and const flags, so a reference node
that shares its children with another tree frees the other tree's children.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static void overwrite_item" -->
```c
static void overwrite_item(cJSON * const root, const cJSON replacement)
{
    if (root == NULL)
    {
        return;
    }

    if (root->string != NULL)
    {
        cJSON_free(root->string);
    }
    if (root->valuestring != NULL)
    {
        cJSON_free(root->valuestring);
    }
    if (root->child != NULL)
    {
        cJSON_Delete(root->child);
    }

    memcpy(root, &replacement, sizeof(cJSON));
}
```

`cJSON_Delete(root->child)` is called whether or not `root` has `cJSON_IsReference` set, and `cJSON_free(root->string)` ignores `cJSON_StringIsConst`. The C twin builds a reference to an array's
children, applies a root-level replace to the reference, and then looks at the original's first child. It uses the poisoning allocator, so the freed node is still readable and shows `0xDD` where a live number
has `0x08`:

<!-- example: 15_cjson_01_overwrite_reference.c -->
```c
// overwrite_item frees root->child without looking at cJSON_IsReference. The reference node shares its child
// with the original, so a root-level "replace" on the reference frees the original's children.
// The allocator is installed through cJSON's own hook API. It gives each block 16 bytes of header (the size)
// and "frees" by filling the block with 0xDD and keeping it, so the freed node can still be looked at.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"

static void *poison_alloc(size_t size) {
    size_t *block = (size_t *)malloc(size + 16);
    block[0] = size;
    return (char *)block + 16;
}

static void poison_free(void *p) {
    if (p != NULL) memset(p, 0xDD, ((size_t *)((char *)p - 16))[0]);
}

int main(void) {
    cJSON_Hooks hooks = { poison_alloc, poison_free };
    cJSON *original, *ref, *patches;
    int status;
    cJSON_InitHooks(&hooks);
    original = cJSON_Parse("[1,2,3]");
    ref = cJSON_CreateArrayReference(original->child);
    patches = cJSON_Parse("[{\"op\":\"replace\",\"path\":\"\",\"value\":9}]");
    status = cJSONUtils_ApplyPatches(ref, patches);
    printf("status %d\n", status);
    printf("first child of the original: type byte 0x%02X (a live number has 0x08)\n", original->child->type & 0xFF);
    return 0;
}
```
<!-- output: 15_cjson_01_overwrite_reference.c -->
```
status 0
first child of the original: type byte 0xDD (a live number has 0x08)
```

**In cJSON-cling.** A reference node does not own its children, it shares them (`createArrayReference` stores the same handle), and the port's `overwriteItem` copies fields and frees nothing:

<!-- source: cjson-cling/src/cjson_item.clg "Item createArrayReference" -->
```cpp
Item createArrayReference(Item child) {
    Item item = new Item();
    item.type = Array;
    item.isReference = true;
    item.child = child;
    return item;
}
```
<!-- source: cjson-cling/src/cjson_utilities.clg "void overwriteItem" -->
```cpp
void overwriteItem(Item root, Item replacement) {
    if (root == nullptr) {
        return;
    }
    root.next = replacement.next;
    root.prev = replacement.prev;
    root.child = replacement.child;
    root.type = replacement.type;
    root.isReference = replacement.isReference;
    root.stringIsConst = replacement.stringIsConst;
    root.valuestring = replacement.valuestring;
    root.hasValuestring = replacement.hasValuestring;
    root.valueint = replacement.valueint;
    root.valuedouble = replacement.valuedouble;
    root.key = replacement.key;
    root.hasKey = replacement.hasKey;
}
```

The same trigger, against the port:

<!-- example: 15_cjson_01_overwrite_reference.clg -->
```cpp
// overwrite_item frees a node's child/key/string without honouring the reference flags. Here a reference
// node shares its children with the original; a root-level replace on the reference must not hurt the original.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item original = parse("[1,2,3]");
    Item ref = createArrayReference(original.child);
    Item patches = parse("[{\"op\":\"replace\",\"path\":\"\",\"value\":9}]");
    int status = applyPatches(ref, patches);
    std::cout << "status " << status << " reference " << printUnformatted(ref) << " original " << printUnformatted(original) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_01_overwrite_reference.clg -->
```
status 0 reference 9 original [1,2,3]
```

**Verdict: cannot happen.** There is nothing to free, and a shared child lives as long as the original's handle does. The `isReference` flag survives as data (a reference string is not editable) and nothing
depends on it for correctness.

### Issue 2: `cJSONUtils_ApplyPatches` reads the next patch after it may have deleted it

**Defect.** When the target tree contains the patch tree, a patch can delete the very array that holds the patches, and the loop then reads the next patch from freed memory.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "CJSON_PUBLIC(int) cJSONUtils_ApplyPatches(cJSON * const object" -->
```c
CJSON_PUBLIC(int) cJSONUtils_ApplyPatches(cJSON * const object, const cJSON * const patches)
{
    const cJSON *current_patch = NULL;
    int status = 0;

    if (!cJSON_IsArray(patches))
    {
        /* malformed patches. */
        return 1;
    }

    if (patches != NULL)
    {
        current_patch = patches->child;
    }

    while (current_patch != NULL)
    {
        status = apply_patch(object, current_patch, false);
        if (status != 0)
        {
            return status;
        }
        current_patch = current_patch->next;
    }

    return 0;
}
```

`current_patch = current_patch->next` runs after `apply_patch` returned, and `apply_patch` may have freed `current_patch` (a `remove` of `/p` frees the array it sits in). The C twin uses the poisoning
allocator, so the read of the freed node finds `0xDD` bytes, and the loop follows a garbage pointer: the process dies with an access violation. With the normal allocator the same program, run once
on this machine, printed `status 2` (the freed memory happened to hold something that made the loop stop). That is what undefined behaviour looks like: the outcome depends on the heap.

<!-- example: 15_cjson_02_patch_deletes_itself.c -->
```c
// ApplyPatches reads current_patch->next after apply_patch ran. Here the first patch removes "/p", the array
// that holds the patches, so the node current_patch points at has been freed. The poisoning allocator (see
// issue 1) makes that read deterministic: the next pointer it finds is 0xDDDDDDDDDDDDDDDD.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"

static void *poison_alloc(size_t size) {
    size_t *block = (size_t *)malloc(size + 16);
    block[0] = size;
    return (char *)block + 16;
}

static void poison_free(void *p) {
    if (p != NULL) memset(p, 0xDD, ((size_t *)((char *)p - 16))[0]);
}

int main(void) {
    cJSON_Hooks hooks = { poison_alloc, poison_free };
    cJSON *doc, *patches;
    cJSON_InitHooks(&hooks);
    doc = cJSON_Parse("{\"p\":[{\"op\":\"remove\",\"path\":\"/p\"},{\"op\":\"add\",\"path\":\"/q\",\"value\":1}]}");
    patches = cJSON_GetObjectItem(doc, "p");
    printf("applying\n");
    fflush(stdout);
    printf("status %d\n", cJSONUtils_ApplyPatches(doc, patches));
    return 0;
}
```

**In cJSON-cling.** `currentPatch` is a handle, and a handle keeps its node alive. Removing `/p` drops the document's handle to the patch array, not the node the loop is standing on:

<!-- source: cjson-cling/src/cjson_utilities.clg "int applyPatchesImpl" -->
```cpp
int applyPatchesImpl(Item object, Item patches, bool caseSensitive) {
    if (!isArray(patches)) {
        // malformed patches
        return 1;
    }
    Item currentPatch = patches.child;
    while (currentPatch != nullptr) {
        int status = applyPatch(object, currentPatch, caseSensitive);
        if (status != 0) {
            return status;
        }
        currentPatch = currentPatch.next;
    }
    return 0;
}
```
<!-- example: 15_cjson_02_patch_deletes_itself.clg -->
```cpp
// ApplyPatches reads the next patch after the patch may have deleted it, when the target tree contains the
// patch tree. The patch removes the very array that holds it.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"p\":[{\"op\":\"remove\",\"path\":\"/p\"},{\"op\":\"add\",\"path\":\"/q\",\"value\":1}]}");
    Item patches = getObjectItem(doc, "p");
    int status = applyPatches(doc, patches);
    std::cout << "status " << status << " doc " << printUnformatted(doc) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_02_patch_deletes_itself.clg -->
```
status 0 doc {"q":1}
```

Both patches are applied, and nothing is read after it was released.

**Verdict: cannot happen.**

### Issue 3: `merge_patch` walks nodes it has already deleted

**Defect.** When target and patch alias each other, `merge_patch` deletes a node and then duplicates it, and keeps iterating a list that the loop itself is cutting up.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static cJSON *merge_patch(cJSON *target" 40 -->
```c
static cJSON *merge_patch(cJSON *target, const cJSON * const patch, const cJSON_bool case_sensitive)
{
    cJSON *patch_child = NULL;

    if (!cJSON_IsObject(patch))
    {
        /* scalar value, array or NULL, just duplicate */
        cJSON_Delete(target);
        return cJSON_Duplicate(patch, 1);
    }

    if (!cJSON_IsObject(target))
    {
        cJSON_Delete(target);
        target = cJSON_CreateObject();
    }

    patch_child = patch->child;
    while (patch_child != NULL)
    {
        if (cJSON_IsNull(patch_child))
        {
            /* NULL is the indicator to remove a value, see RFC7396 */
            if (case_sensitive)
            {
                cJSON_DeleteItemFromObjectCaseSensitive(target, patch_child->string);
            }
            else
            {
                cJSON_DeleteItemFromObject(target, patch_child->string);
            }
        }
        else
        {
            cJSON *replace_me = NULL;
            cJSON *replacement = NULL;

            if (case_sensitive)
            {
                replace_me = cJSON_DetachItemFromObjectCaseSensitive(target, patch_child->string);
    ...
```

For `merge_patch(both, both)` the recursion reaches the number `b`, calls `cJSON_Delete(target)` and then `cJSON_Duplicate(patch, 1)` on the same, freed, node. The C twin (poisoning allocator) dies on the
`0xDD` fill. With the normal allocator the same call printed `{"c":2,"d":null,"a":{}}` on this machine, one run: the member `b` is missing because the duplicate read freed memory. Not a result anyone can rely on.

<!-- example: 15_cjson_03_merge_patch_alias.c -->
```c
// merge_patch(target, patch) with target and patch the same tree: for the number "b" it calls
// cJSON_Delete(target) and then cJSON_Duplicate(patch), and those are the same node. The poisoning allocator
// (see issue 1) turns the read of the freed node into a read of 0xDD bytes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"

static void *poison_alloc(size_t size) {
    size_t *block = (size_t *)malloc(size + 16);
    block[0] = size;
    return (char *)block + 16;
}

static void poison_free(void *p) {
    if (p != NULL) memset(p, 0xDD, ((size_t *)((char *)p - 16))[0]);
}

int main(void) {
    cJSON_Hooks hooks = { poison_alloc, poison_free };
    cJSON *both, *result;
    cJSON_InitHooks(&hooks);
    both = cJSON_Parse("{\"a\":{\"b\":1},\"c\":2,\"d\":null}");
    printf("merging\n");
    fflush(stdout);
    result = cJSONUtils_MergePatch(both, both);
    printf("result %s\n", cJSON_PrintUnformatted(result));
    return 0;
}
```

**In cJSON-cling.** The port never deletes the target; a replaced node is simply no longer referenced by the result:

<!-- source: cjson-cling/src/cjson_utilities.clg "Item mergePatchImpl" -->
```cpp
Item mergePatchImpl(Item target, Item patch, bool caseSensitive) {
    if (!isObject(patch)) {
        // scalar, array or NULL: just duplicate
        return duplicate(patch, true);
    }
    Item result = target;
    if (!isObject(result)) {
        result = createObject();
    }
    Item patchChild = patch.child;
    while (patchChild != nullptr) {
        if (isNull(patchChild)) {
            // null is the indicator to remove a value (RFC 7396)
            if (caseSensitive) {
                deleteItemFromObjectCaseSensitive(result, patchChild.key);
            } else {
                deleteItemFromObject(result, patchChild.key);
            }
        } else {
            Item replaceMe = nullptr;
            if (caseSensitive) {
                replaceMe = detachItemFromObjectCaseSensitive(result, patchChild.key);
            } else {
                replaceMe = detachItemFromObject(result, patchChild.key);
            }
            Item replacement = mergePatchImpl(replaceMe, patchChild, caseSensitive);
            if (replacement == nullptr) {
                return nullptr;
            }
            addItemToObject(result, patchChild.key, replacement);
        }
        patchChild = patchChild.next;
    }
    return result;
}
```
<!-- example: 15_cjson_03_merge_patch_alias.clg -->
```cpp
// merge_patch keeps walking nodes it already deleted when target and patch alias each other.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item both = parse("{\"a\":{\"b\":1},\"c\":2,\"d\":null}");
    Item result = mergePatch(both, both);
    std::cout << "result " << printUnformatted(result) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_03_merge_patch_alias.clg -->
```
result {"c":2,"d":null,"a":{"b":1}}
```

The result is defined and no freed node is touched. It is not what RFC 7396 would give for two separate copies (the `null` member `d` is still there), because the walk modifies the list it is walking; the
aliasing question is a logic question, which the language does not answer. What the language removes is the use after free.

**Verdict: cannot happen** (the use after free). The merge of a tree with itself is still a questionable operation.

### Issue 4: `ReplaceItemViaPointer` frees the replaced item and can replace a node with its own parent

**Defect.** The replaced item is deleted while the library may still point at it, and nothing stops the replacement from being the item's own parent.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer" -->
```c
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement)
{
    if ((parent == NULL) || (parent->child == NULL) || (replacement == NULL) || (item == NULL))
    {
        return false;
    }

    if (replacement == item)
    {
        return true;
    }

    replacement->next = item->next;
    replacement->prev = item->prev;

    if (replacement->next != NULL)
    {
        replacement->next->prev = replacement;
    }
    if (parent->child == item)
    {
        if (parent->child->prev == parent->child)
        {
            replacement->prev = replacement;
        }
        parent->child = replacement;
    }
    else
    {   /*
         * To find the last item in array quickly, we use prev in array.
         * We can't modify the last item's next pointer where this item was the parent's child
         */
        if (replacement->prev != NULL)
        {
            replacement->prev->next = replacement;
        }
        if (replacement->next == NULL)
        {
            parent->child->prev = replacement;
        }
    }

    item->next = NULL;
    item->prev = NULL;
    cJSON_Delete(item);

    return true;
}
```

With `replacement == parent`, `parent->child` becomes `parent`: the tree now contains itself, the old child is deleted, and the function returns true. The C twin shows that state and then prints the tree,
which recurses until the stack is gone:

<!-- example: 15_cjson_04_replace_with_ancestor.c -->
```c
// ReplaceItemViaPointer with the item's own parent as the replacement: the parent becomes its own child,
// and the function reports success.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *root = cJSON_Parse("{\"k\":{\"x\":1}}");
    cJSON *k = cJSON_GetObjectItem(root, "k");
    cJSON_bool ok = cJSON_ReplaceItemViaPointer(root, k, root);
    printf("replace returned %d, root->child == root: %d\n", (int)ok, root->child == root);
    fflush(stdout);
    printf("printed %s\n", cJSON_PrintUnformatted(root));
    return 0;
}
```
<!-- output: 15_cjson_04_replace_with_ancestor.c -->
```
replace returned 1, root->child == root: 1
```

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_item.clg "bool replaceItemViaPointer" -->
```cpp
bool replaceItemViaPointer(Item parent, Item item, Item replacement) {
    if (parent == nullptr || parent.child == nullptr || replacement == nullptr || item == nullptr) {
        return false;
    }
    if (replacement == item) {
        return true;
    }
    // Hardening beyond upstream (issues 4 and 6): a node cannot replace its own parent -- that would
    // make the tree contain itself. Deeper cycles (a replacement that is a more distant ancestor)
    // are not detected: nodes do not know their parent.
    if (replacement == parent) {
        return false;
    }
    replacement.next = item.next;
    replacement.prev = item.prev;
    if (replacement.next != nullptr) {
        replacement.next.prev = replacement;
    }
    if (parent.child == item) {
        if (parent.child.prev == parent.child) {
            replacement.prev = replacement;
        }
        parent.child = replacement;
    } else {
        // the last item's prev is kept up to date through the first child
        if (replacement.prev != nullptr) {
            replacement.prev.next = replacement;
        }
        if (replacement.next == nullptr) {
            parent.child.prev = replacement;
        }
    }
    item.next = nullptr;
    item.prev = nullptr;
    return true;
}
```

Two different things happen here and they should not be confused. The *free* of a still-referenced node cannot be written: `item` is detached and simply dropped when its last handle goes. The *cycle* is
refused by the three lines `if (replacement == parent) { return false; }`, which are library code, not language: CeeLing does not detect ownership cycles, and nodes do not know their parent, so a replacement
that is a more distant ancestor is not detected (that is issue 6, second half).

<!-- example: 15_cjson_04_replace_with_ancestor.clg -->
```cpp
// ReplaceItemViaPointer frees the replaced item while the library still points at it, when the replacement
// is the item's own parent. (In C this is a use-after-free; here nothing is freed by hand, and the library
// additionally refuses to build the cycle.)
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
int main() {
    Item root = parse("{\"k\":{\"x\":1}}");
    Item k = getObjectItem(root, "k");
    bool ok = replaceItemViaPointer(root, k, root);
    std::cout << "replace returned " << ok << " printed " << printUnformatted(root) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_04_replace_with_ancestor.clg -->
```
replace returned 0 printed {"k":{"x":1}}
```

**Verdict: cannot happen** for the use after free; the direct cycle is refused by the library's own check, not by the language.

### Issue 7: `cJSON_ParseWithOpts` and `cJSON_GetErrorPtr` keep a stale pointer

**Defect.** `ParseWithOpts(NULL, ...)` returns early without resetting the saved error state, so `cJSON_GetErrorPtr` can return a pointer into a buffer the caller has already freed.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "static error global_error" -->
```c
static error global_error = { NULL, 0 };

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return (const char*) (global_error.json + global_error.position);
}
```
<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts" -->
```c
CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    size_t buffer_length;

    if (NULL == value)
    {
        return NULL;
    }

    /* Adding null character size due to require_null_terminated. */
    buffer_length = strlen(value) + sizeof("");

    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}
```

The error is a pointer into the *caller's* text plus one global record. The reset happens inside `cJSON_ParseWithLengthOpts`, which the `NULL` path never reaches. The C twin parses text in a heap
buffer, frees the buffer, calls `cJSON_ParseWithOpts(NULL)` and checks that the saved pointer is unchanged. It compares the pointer; reading through it would be the use after free.

<!-- example: 15_cjson_07_stale_error_pointer.c -->
```c
// cJSON_ParseWithOpts(NULL, ...) returns before the code that resets the saved error, so cJSON_GetErrorPtr()
// still points into the buffer of the earlier call, which the caller has since freed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
int main(void) {
    char *buffer = (char *)malloc(8);
    const char *old_error;
    cJSON *item;
    strcpy(buffer, "[1,");
    item = cJSON_Parse(buffer);
    old_error = cJSON_GetErrorPtr();
    printf("failed at %d\n", (int)(old_error - buffer));
    free(buffer);                                   /* the caller is done with its text */
    item = cJSON_ParseWithOpts(NULL, NULL, 0);      /* "nothing to parse": returns early */
    printf("after cJSON_ParseWithOpts(NULL): item=%d, the error pointer is still the old one: %d\n",
           item != NULL, cJSON_GetErrorPtr() == old_error);
    return 0;
}
```
<!-- output: 15_cjson_07_stale_error_pointer.c -->
```
failed at 3
after cJSON_ParseWithOpts(NULL): item=0, the error pointer is still the old one: 1
```

**In cJSON-cling.** The error is a value in the result, not a pointer into the caller's text, and there is no saved state:

<!-- source: cjson-cling/src/cjson_parse.clg "class ParseResult" -->
```cpp
class ParseResult {
public:
    Item item;
    int errorPos;
    int end;
    ParseResult() {
        item = nullptr;
        errorPos = 0;
        end = 0;
    }
};
```
<!-- example: 15_cjson_07_stale_error_pointer.clg -->
```cpp
// ParseWithOpts/GetErrorPtr return early on NULL input without resetting the saved error state, so the
// error pointer can reference a buffer the caller already freed. Here the error position is a value in the
// result, not a pointer into the caller's buffer, and there is no saved state to go stale.
#include "cjson_item.clh"
#include "cjson_parse.clh"
int main() {
    ParseResult first = parseWithOptsResult("[1,", false);
    std::cout << "failed at " << first.errorPos << "; then empty input: ";
    ParseResult second = parseWithOptsResult("", false);
    std::cout << "item=" << (second.item != nullptr) << " errorPos=" << second.errorPos << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_07_stale_error_pointer.clg -->
```
failed at 3; then empty input: item=0 errorPos=0
```

There is no `cJSON_GetErrorPtr` in the port (`docs/api-differences.md`). That is a change of interface made while porting, and the language is what made it natural: the old design needs a file-scope variable, which
CeeLing rejects (issue 32).

**Verdict: cannot happen.**

## Part 2. Null bugs: issues 5, 11, 12, 13

C does not say whether a `NULL` is allowed, so each function guesses. Two mechanisms close this in CeeLing, because there are two kinds of null.

**A null object handle (5, 11): a run-time check on every dereference.** Every `->` the generator emits for a member access or a method call goes through `cling::nn(handle, "line N")`, which prints the `.clg`
line and aborts instead of crashing or corrupting memory ([chapter 7](07-run-time-checks.md)). It is on by default (`--no-null-checks` turns it off). In miniature, with the generated code:

<!-- example: 15_cjson_05_null_check_miniature.clg -->
```cpp
// Issue 5 in miniature, without the library: a parent that has no child, and a write through the child.
class Item {
public:
    Item child;                        // the first child, or empty
    Item next;
    int v;
    Item(int x) { v = x; }
    void unlinkFirstChild() {
        child.next = nullptr;          // C: parent->child->next = NULL, with no test that child exists
    }
};

int main() {
    Item parent = new Item(1);         // a parent with no children
    parent.unlinkFirstChild();
    std::cout << "not reached" << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_05_null_check_miniature.clg -->
```
cling: null dereference at line 9
```
<!-- generated: 15_cjson_05_null_check_miniature.clg "void Item::unlinkFirstChild()" -->
```cpp
void Item::unlinkFirstChild() {
    cling::nn(this->child, "line 9")->next = nullptr;
}
```

**A null string (12) and a null allocation (13): the value does not exist.** A CeeLing `string` is a value and is never `NULL`; there is no `malloc` whose result could be. The port's functions are written
*faithfully* at these places: it does not add its own `if (object == nullptr)` guard where upstream lacks one (a guard would hide the bug being demonstrated), so what happens is the language's doing.

### Issue 5: `cJSON_DetachItemViaPointer` writes through a `NULL` `parent->child`

**Defect.** The guard `item != parent->child && item->prev == NULL` does not cover an item whose `prev` is set while its claimed parent has no child; the last branch then writes through `parent->child`.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer" -->
```c
CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item)
{
    if ((parent == NULL) || (item == NULL) || (item != parent->child && item->prev == NULL))
    {
        return NULL;
    }

    if (item != parent->child)
    {
        /* not the first element */
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        /* not the last element */
        item->next->prev = item->prev;
    }

    if (item == parent->child)
    {
        /* first element */
        parent->child = item->next;
    }
    else if (item->next == NULL)
    {
        /* last element */
        parent->child->prev = item->prev;
    }

    /* make sure the detached item doesn't point anywhere anymore */
    item->prev = NULL;
    item->next = NULL;

    return item;
}
```
<!-- example: 15_cjson_05_detach_null_child.c -->
```c
// DetachItemViaPointer: the claimed parent has no child, but the item has a prev. The check
// (item != parent->child && item->prev == NULL) passes, and the last branch writes through parent->child.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *parent = cJSON_CreateArray();
    cJSON *other = cJSON_CreateNumber(1.0);
    cJSON *item = cJSON_CreateNumber(2.0);
    item->prev = other;
    printf("detaching\n");
    fflush(stdout);
    cJSON_DetachItemViaPointer(parent, item);
    printf("survived\n");
    return 0;
}
```

The C twin dies with a segmentation fault at `parent->child->prev = item->prev`. (Exit status and message vary by system; the write is to a small address near zero.)

**In cJSON-cling.** The port's function is the same code, line for line:

<!-- source: cjson-cling/src/cjson_item.clg "Item detachItemViaPointer" -->
```cpp
Item detachItemViaPointer(Item parent, Item item) {
    if (parent == nullptr || item == nullptr) {
        return nullptr;
    }
    if (item != parent.child && item.prev == nullptr) {
        return nullptr;
    }
    if (item != parent.child) {
        // not the first element
        item.prev.next = item.next;
    }
    if (item.next != nullptr) {
        // not the last element
        item.next.prev = item.prev;
    }
    if (item == parent.child) {
        parent.child = item.next;
    } else if (item.next == nullptr) {
        // last element
        parent.child.prev = item.prev;
    }
    // make sure the detached item doesn't point anywhere anymore
    item.prev = nullptr;
    item.next = nullptr;
    return item;
}
```
<!-- example: 15_cjson_05_detach_null_child.clg -->
```cpp
// DetachItemViaPointer writes through parent->child without checking it is non-NULL: an item whose
// prev is set but whose claimed parent has no child.
#include "cjson_item.clh"
int main() {
    Item parent = createArray();
    Item other = createNumber(1.0);
    Item item = createNumber(2.0);
    item.prev = other;
    Item r = detachItemViaPointer(parent, item);
    std::cout << "survived " << (r != nullptr) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_05_detach_null_child.clg -->
```
cling: null dereference at line 439
```

The line it names is `parent.child.prev = item.prev;` in `src/cjson_item.clg`. The exit status is 3.

**Verdict: stopped at run time.** The program still stops. A run-time check turns a crash or a silent write into a clean, located abort; it does not make the bug's input work.

### Issue 11: `apply_patch` dereferences a `NULL` object

**Defect.** A root-level `add` or `replace` applied to a `NULL` object: `overwrite_item` returns early for `NULL`, then `apply_patch` reads `object->string` without a test.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "if (path->valuestring[0] == '\0')" -->
```c
    if (path->valuestring[0] == '\0')
    {
        if (opcode == REMOVE)
        {
            static const cJSON invalid = { NULL, NULL, NULL, cJSON_Invalid, NULL, 0, 0, NULL};

            overwrite_item(object, invalid);

            status = 0;
            goto cleanup;
        }

        if ((opcode == REPLACE) || (opcode == ADD))
        {
            value = get_object_item(patch, "value", case_sensitive);
            if (value == NULL)
            {
                /* missing "value" for add/replace. */
                status = 7;
                goto cleanup;
            }

            value = cJSON_Duplicate(value, 1);
            if (value == NULL)
            {
                /* out of memory for add/replace. */
                status = 8;
                goto cleanup;
            }

            overwrite_item(object, *value);

            /* delete the duplicated value */
            cJSON_free(value);
            value = NULL;

            /* the string "value" isn't needed */
            if (object->string != NULL)
            {
                cJSON_free(object->string);
                object->string = NULL;
            }

            status = 0;
            goto cleanup;
        }
    }
```
<!-- example: 15_cjson_11_patch_null_root.c -->
```c
// A root-level "add" applied to a NULL object. overwrite_item returns early for NULL, then apply_patch
// reads object->string without a test.
#include <stdio.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *patches = cJSON_Parse("[{\"op\":\"add\",\"path\":\"\",\"value\":1}]");
    printf("applying\n");
    fflush(stdout);
    printf("survived status %d\n", cJSONUtils_ApplyPatches(NULL, patches));
    return 0;
}
```

The twin crashes at `object->string` with `object == NULL`.

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_utilities.clg "if (path.valuestring.length() == 0) {" -->
```cpp
    if (path.valuestring.length() == 0) {
        if (opcode == 2) {
            Item invalid = new Item();
            overwriteItem(object, invalid);
            return 0;
        }
        if (opcode == 3 || opcode == 1) {
            value = getObjectItemImpl(patch, "value", caseSensitive);
            if (value == nullptr) {
                return 7;
            }
            value = duplicate(value, true);
            if (value == nullptr) {
                return 8;
            }
            overwriteItem(object, value);
            // the key of "value" isn't needed (upstream dereferences `object` here without a NULL test:
            // a NULL root reaches this line, and is caught by the null check)
            if (object.hasKey) {
                object.key = "";
                object.hasKey = false;
            }
            return 0;
        }
    }
```
<!-- example: 15_cjson_11_patch_null_root.clg -->
```cpp
// apply_patch dereferences the target's key without a NULL check when a root-level add/replace is applied to
// a NULL object.
#include "cjson_item.clh"
#include "cjson_parse.clh"
#include "cjson_utilities.clh"
int main() {
    Item patches = parse("[{\"op\":\"add\",\"path\":\"\",\"value\":1}]");
    Item nothing = nullptr;
    int status = applyPatches(nothing, patches);
    std::cout << "survived status " << status << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_11_patch_null_root.clg -->
```
cling: null dereference at line 607
```

The comment in the port's source says what the line is: upstream's unguarded dereference, kept, so that the language's check is what catches it.

**Verdict: stopped at run time** (abort, exit 3, the line of `object.hasKey`).

### Issue 12: a string node whose value is `NULL`

**Defect.** `apply_patch` and `detach_path` read a string node's value after checking only its *type*. A node of type string with a `NULL` value (for example `cJSON_CreateStringReference(NULL)`) passes the
check and then crashes them.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(cJSON_bool) cJSON_IsString" -->
```c
CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_String;
}
```
<!-- source: cjson-cling/upstream/cJSON_Utils.c "path = get_object_item(patch," -->
```c
    path = get_object_item(patch, "path", case_sensitive);
    if (!cJSON_IsString(path))
    {
        /* malformed patch. */
        status = 2;
        goto cleanup;
    }
```
<!-- example: 15_cjson_12_string_without_value.c -->
```c
// apply_patch checks that "path" is a string (cJSON_IsString looks at the type only) and then reads
// path->valuestring[0]. A string node built with cJSON_CreateStringReference(NULL) has type String and no text.
#include <stdio.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1}");
    cJSON *patch = cJSON_CreateObject();
    cJSON *patches = cJSON_CreateArray();
    cJSON_AddStringToObject(patch, "op", "remove");
    cJSON_AddItemToObject(patch, "path", cJSON_CreateStringReference(NULL));
    cJSON_AddItemToArray(patches, patch);
    printf("applying\n");
    fflush(stdout);
    printf("status %d\n", cJSONUtils_ApplyPatches(doc, patches));
    return 0;
}
```

After the type check, the first use is `path->valuestring[0]` (quoted under issue 11), and the twin dies there.

**In cJSON-cling.** A `string` is a value and cannot be null, so `getObjectItem(...).valuestring` always has some text. The "no value" state still exists as a flag (`hasValuestring`), and a node can be put
in it by hand, because the fields are public. What the port does with it:

<!-- example: 15_cjson_12_string_without_value.clg -->
```cpp
// apply_patch/detach_path read a string node's value after checking only its type. Here a string node with
// no text (type String, hasValuestring false) is the "path" of a patch. In the port the missing text reads as
// the empty string, which is the path of the root: "remove the root".
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"a\":1}");
    Item path = new Item();
    path.type = String;
    Item patch = createObject();
    addItemToObject(patch, "op", createString("remove"));
    addItemToObject(patch, "path", path);
    Item patches = createArray();
    addItemToArray(patches, patch);
    int status = applyPatches(doc, patches);
    string text = printUnformatted(doc);
    if (text.length() == 0) {
        text = "<nothing to print>";
    }
    std::cout << "status " << status << " doc " << text << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_12_string_without_value.clg -->
```
status 0 doc <nothing to print>
```

**Verdict: cannot happen**, as a crash. This is the one place where the port does something arguably worse than C: it treats the malformed node as the empty path, which means "the root", and the
`remove` empties the document. A CeeLing string cannot be null, so the library had to say something for "no value"; the choice is recorded in `docs/api-differences.md`.

### Issue 13: unchecked `cJSON_malloc` in `cJSON_Utils.c`

**Defect.** Several allocation sites in `cJSON_Utils.c` write to the result of `cJSON_malloc` without testing it for `NULL`.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "if (cJSON_IsArray(object))" -->
```c
            if (cJSON_IsArray(object))
            {
                /* reserve enough memory for a 64 bit integer + '/' and '\0' */
                unsigned char *full_pointer = (unsigned char*)cJSON_malloc(strlen((char*)target_pointer) + 20 + sizeof("/"));
                /* check if conversion to unsigned long is valid
                 * This should be eliminated at compile time by dead code elimination
                 * if size_t is an alias of unsigned long, or if it is bigger */
                if (child_index > ULONG_MAX)
                {
                    cJSON_free(target_pointer);
                    cJSON_free(full_pointer);
                    return NULL;
                }
                sprintf((char*)full_pointer, "/%lu%s", (unsigned long)child_index, target_pointer); /* /<array_index><path> */
                cJSON_free(target_pointer);

                return (char*)full_pointer;
            }
```

The article names more sites than this one (`grep cJSON_malloc cJSON_Utils.c` finds six calls; five of them use the result without a test, and the sixth is the `strdup` helper, which tests it). To reproduce one deterministically the C twin installs an allocator that fails (through `cJSON_InitHooks`)
and asks for a pointer. It dies in `sprintf` into the `NULL` that `cJSON_malloc` returned:

<!-- example: 15_cjson_13_alloc_unchecked.c -->
```c
// cJSONUtils_FindPointerFromObjectTo writes into the result of cJSON_malloc without checking it. cJSON_malloc
// calls the allocator hook, so an allocator that fails (installed with cJSON_InitHooks, here failing from the
// second allocation after the parse) shows what an out-of-memory machine would do.
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "cJSON_Utils.h"

static int allocations = 0;
static int fail_from = 1000000;

static void *failing_alloc(size_t size) {
    return allocations++ >= fail_from ? NULL : malloc(size);
}

int main(void) {
    cJSON_Hooks hooks = { failing_alloc, free };
    cJSON *doc, *target;
    cJSON_InitHooks(&hooks);
    doc = cJSON_Parse("{\"a\":[1,2]}");
    target = cJSONUtils_GetPointer(doc, "/a/1");
    fail_from = allocations + 1;         /* the next allocation (an empty string) succeeds, the one after it fails */
    printf("looking for the pointer\n");
    fflush(stdout);
    printf("pointer %s\n", cJSONUtils_FindPointerFromObjectTo(doc, target));
    return 0;
}
```

**In cJSON-cling.** There is no `malloc`. The text is built from `string` values:

<!-- source: cjson-cling/src/cjson_utilities.clg "PointerResult findPointerFromObjectTo" -->
```cpp
PointerResult findPointerFromObjectTo(Item object, Item target) {
    PointerResult result = new PointerResult();
    if (object == nullptr || target == nullptr) {
        return result;
    }
    if (object == target) {
        result.found = true;
        return result;
    }
    int childIndex = 0;
    Item currentChild = object.child;
    while (currentChild != nullptr) {
        PointerResult sub = findPointerFromObjectTo(currentChild, target);
        if (sub.found) {
            if (isArray(object)) {
                result.found = true;
                result.text = "/" + intToString(int64(childIndex)) + sub.text;
                return result;
            }
            if (isObject(object)) {
                result.found = true;
                result.text = "/" + encodeAsPointer(currentChild.key) + sub.text;
                return result;
            }
            // reached a leaf of the tree, found nothing
            return result;
        }
        currentChild = currentChild.next;
        childIndex = childIndex + 1;
    }
    return result;
}
```
<!-- example: 15_cjson_13_alloc_unchecked.clg -->
```cpp
// Several allocation sites in cJSON_Utils write to the result of cJSON_malloc without checking for NULL.
// There is no malloc in the port: strings are std::string, nodes come from make_shared. (An allocation
// failure throws std::bad_alloc, which ends the program; it is never an unchecked NULL write.) The
// function the article names is exercised here.
#include "cjson_item.clh"
#include "cjson_parse.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"a\":[1,2]}");
    Item target = getPointer(doc, "/a/1");
    PointerResult p = findPointerFromObjectTo(doc, target);
    std::cout << "pointer " << p.text << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_13_alloc_unchecked.clg -->
```
pointer /a/1
```

An allocation that fails throws `std::bad_alloc` from the C++ runtime, which ends the program; it is never a write through `NULL`. I did not simulate an out-of-memory machine for the port, so that last
sentence is the C++ standard's behaviour, not something this chapter ran.

**Verdict: cannot happen** (as an unchecked write). Running out of memory is still running out of memory.

## Part 3. Recursion and cost: issues 6, 8, 9, 10

The answer here is *partly*, and the "partly" is the useful part.

* **Freeing a deep tree (the `Delete` half of 9) is solved by the compiler.** A class that owns another object of its own type (`next`, `child`) gets a generated destructor that empties the chain in a loop,
  so the depth is 1. This was found by probe: a 1,000,000-node chain printed its result and then crashed on scope exit, because `shared_ptr` destruction recurses once per node. In miniature, with the
  destructor the compiler wrote:

<!-- example: 15_cjson_09_drain_destructor.clg -->
```cpp
// What the port's Item relies on, in miniature: a class that owns another object of its own type gets a destructor
// that empties the chain in a loop. Without it the default destructor releases one node per stack frame.
class Node {
public:
    Node next;
    int value;
    Node(int v) { value = v; }
};

int main() {
    Node head = new Node(0);
    Node cur = head;
    int i = 1;
    while (i < 1000000) {
        Node n = new Node(i);
        cur.next = n;
        cur = n;
        i = i + 1;
    }
    std::cout << i << " nodes built" << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_09_drain_destructor.clg -->
```
1000000 nodes built
```
<!-- generated: 15_cjson_09_drain_destructor.clg "Node::~Node()" -->
```cpp
Node::~Node() {
    if (!next) return;
    std::vector<std::shared_ptr<Node>> pending;
    if (next) pending.push_back(std::move(next));
    while (!pending.empty()) {
        std::shared_ptr<Node> node = std::move(pending.back());
        pending.pop_back();
        if (node.use_count() != 1) continue;
        if (node->next) pending.push_back(std::move(node->next));
    }
}
```

* **Putting a node inside itself (6, 4) is one line of library code, not the language.** CeeLing does not detect ownership cycles.
* **Everything else (8, the print half of 9, indirect 6, 10) is not addressed.** Printing, comparing and duplicating still recurse once per level, and CeeLing has no stack-depth check.

These say the same thing from two sides: CeeLing makes the *memory* side of a cycle or a deep tree safe and leaves the *resource* side alone. A `shared_ptr` cycle is a leak, not a corruption; a stack
overflow is a crash, not a corruption. Both are outside the contract ([chapter 13](13-contract-and-threats.md)), and a service would still be taken down by the second.

### Issue 6: `cJSON_ReplaceItemInArray` puts an array inside itself

**Defect.** `ReplaceItemInArray(a, 0, a)` makes the array its own child, reports success, and `Delete` and `Print` then recurse forever.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray" -->
```c
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return false;
    }

    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}
```

The function is a thin wrapper around `cJSON_ReplaceItemViaPointer` (issue 4), with the same missing check. The C twin makes the self-cycle and prints it; a second twin builds the cycle one step away
(`a` contains `b`, `b` contains `a`), which no check on a single call can see. Both die by exhausting the stack:

<!-- example: 15_cjson_06_array_into_itself.c -->
```c
// ReplaceItemInArray(a, 0, a): the array replaces its own first element, becomes its own child, and the call
// reports success.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *a = cJSON_Parse("[1,2,3]");
    cJSON_bool ok = cJSON_ReplaceItemInArray(a, 0, a);
    printf("replace returned %d, a->child == a: %d\n", (int)ok, a->child == a);
    fflush(stdout);
    printf("printed %s\n", cJSON_PrintUnformatted(a));
    return 0;
}
```
<!-- example: 15_cjson_06b_cycle_through_two_arrays.c -->
```c
// The same cycle one step further away: a contains b, b contains a. Nothing in the API refuses it.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *a = cJSON_CreateArray();
    cJSON *b = cJSON_CreateArray();
    cJSON_AddItemToArray(a, b);
    cJSON_AddItemToArray(b, a);
    printf("built\n");
    fflush(stdout);
    printf("printed %s\n", cJSON_PrintUnformatted(a));
    return 0;
}
```

**In cJSON-cling.** The direct form is refused by the three lines of library code from issue 4:

<!-- source: cjson-cling/src/cjson_item.clg "if (replacement == parent) {" -->
```cpp
    if (replacement == parent) {
        return false;
    }
```
<!-- example: 15_cjson_06_array_into_itself.clg -->
```cpp
// ReplaceItemInArray lets an array be put inside itself and reports success; Delete then recurses forever.
// The library now refuses (one line of hardening); there is nothing in CeeLing's own checks that would have.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
int main() {
    Item a = parse("[1,2,3]");
    bool ok = replaceItemInArray(a, 0, a);
    std::cout << "replace returned " << ok << " printed " << printUnformatted(a) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_06_array_into_itself.clg -->
```
replace returned 0 printed [1,2,3]
```

The indirect form is not refused by anything, and printing it exhausts the stack, exactly as in C (the process dies; the exit status on this machine was a stack-overflow code):

<!-- example: 15_cjson_06b_cycle_through_two_arrays.clg -->
```cpp
// The same cycle built one step further away: a contains b, b contains a. The library cannot see it (nodes
// do not know their parent), CeeLing does not check for it, and printing exhausts the stack.
#include "cjson_item.clh"
#include "cjson_print.clh"
int main() {
    Item a = createArray();
    Item b = createArray();
    addItemToArray(a, b);
    addItemToArray(b, a);
    string text = printUnformatted(a);
    std::cout << text.length() << std::endl;
    return 0;
}
```

**Verdict: partly.** The direct form is refused, by library code. The indirect cycle is still there; the sanitizer run reports `AddressSanitizer: stack-overflow` for this program (the internal notes).

### Issue 8: `cJSON_Duplicate` cap too high for the stack

**Defect.** `Duplicate` stops at `CJSON_CIRCULAR_LIMIT` (10,000) levels, which is a number of recursive calls, not a number of bytes of stack. On a small stack (a thread, an embedded target) 10,000 frames
can run out before the cap triggers.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "/* Walk the ->next chain for the child. */" -->
```c
    /* Walk the ->next chain for the child. */
    child = item->child;
    while (child != NULL)
    {
        if(depth >= CJSON_CIRCULAR_LIMIT) {
            goto fail;
        }
        newchild = cJSON_Duplicate_rec(child, depth + 1, true); /* Duplicate (with recurse) each item in the ->next chain */
        if (!newchild)
        {
            goto fail;
        }
        if (next != NULL)
        {
            /* If newitem->child already set, then crosswire ->prev and ->next and move on */
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        }
        else
        {
            /* Set newitem->child and move to it */
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }
```

I could **not** reproduce the defect here: on this machine's default stack 9,990 levels duplicate fine, and one level past the limit is refused. The twins show exactly that, which pins down the
limit and says nothing about smaller stacks:

<!-- example: 15_cjson_08_duplicate_deep.c -->
```c
// cJSON_Duplicate recurses once per level and stops at CJSON_CIRCULAR_LIMIT (10000). The limit counts levels,
// not bytes of stack, so whether 10000 nested calls fit depends on the build and the machine. On this machine
// the default stack is enough for 9990 levels, so this program does not show the defect; it shows that the
// limit is a number of levels (the chapter explains what that means for a smaller stack).
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *top = cJSON_CreateArray();
    cJSON *cur = top;
    int i;
    for (i = 0; i < 9990; i++) {
        cJSON *next = cJSON_CreateArray();
        cJSON_AddItemToArray(cur, next);
        cur = next;
    }
    printf("duplicated %d\n", cJSON_Duplicate(top, 1) != NULL);
    return 0;
}
```
<!-- example: 15_cjson_08b_duplicate_limit.c -->
```c
// One level past CJSON_CIRCULAR_LIMIT: Duplicate returns NULL, as in the port.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *top = cJSON_CreateArray();
    cJSON *cur = top;
    int i;
    for (i = 0; i < 10001; i++) {
        cJSON *next = cJSON_CreateArray();
        cJSON_AddItemToArray(cur, next);
        cur = next;
    }
    printf("duplicate of 10001 levels: %s\n", cJSON_Duplicate(top, 1) == NULL ? "refused" : "accepted");
    return 0;
}
```

**In cJSON-cling.** The port has the same cap, the same recursion, the same behaviour:

<!-- source: cjson-cling/src/cjson_item.clg "Item child = item.child;" -->
```cpp
    Item child = item.child;
    Item last = nullptr;
    Item newchild = nullptr;
    while (child != nullptr) {
        if (depth >= 10000) {
            return nullptr;
        }
        newchild = duplicateRec(child, depth + 1, true);
        if (newchild == nullptr) {
            return nullptr;
        }
        if (last != nullptr) {
            last.next = newchild;
            newchild.prev = last;
            last = newchild;
        } else {
            newitem.child = newchild;
            last = newchild;
        }
        child = child.next;
    }
```
<!-- example: 15_cjson_08_duplicate_deep.clg -->
```cpp
// Duplicate's recursion cap (CJSON_CIRCULAR_LIMIT = 10000) against the real stack: build a chain
// 9990 arrays deep through the API (the parser would refuse > 1000) and duplicate it.
#include "cjson_item.clh"
int main() {
    Item top = createArray();
    Item cur = top;
    int i = 0;
    while (i < 9990) {
        Item next = createArray();
        addItemToArray(cur, next);
        cur = next;
        i = i + 1;
    }
    Item copy = duplicate(top, true);
    std::cout << "duplicated " << (copy != nullptr) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_08_duplicate_deep.clg -->
```
duplicated 1
```
<!-- example: 15_cjson_08b_duplicate_limit.clg -->
```cpp
// Duplicate stops at CJSON_CIRCULAR_LIMIT (10000) levels and returns NULL; the issue is that 10000 frames can
// already exhaust a small stack. Here one level past the limit is refused (same as upstream), and 9990 levels
// is accepted (08_duplicate_deep) on this machine's default stack.
#include "cjson_item.clh"
int main() {
    Item top = createArray();
    Item cur = top;
    int i = 0;
    while (i < 10001) {
        Item next = createArray();
        addItemToArray(cur, next);
        cur = next;
        i = i + 1;
    }
    Item copy = duplicate(top, true);
    if (copy == nullptr) {
        std::cout << "duplicate of 10001 levels: refused" << std::endl;
    } else {
        std::cout << "duplicate of 10001 levels: accepted" << std::endl;
    }
    return 0;
}
```
<!-- output: 15_cjson_08b_duplicate_limit.clg -->
```
duplicate of 10001 levels: refused
```

**Verdict: partly, and really "unchanged".** The cap is the only guard, in C and in the port. Whether it is enough depends on the stack size; no CeeLing check is involved.

### Issue 9: `cJSON_Delete` (and a patch `copy`) build and free trees of unbounded depth

**Defect.** `cJSON_Delete` has no depth bound, and a JSON Patch `copy` can build trees deep enough to overflow the stack.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)" -->
```c
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL)
    {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && (item->child != NULL))
        {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL))
        {
            global_hooks.deallocate(item->valuestring);
            item->valuestring = NULL;
        }
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
        {
            global_hooks.deallocate(item->string);
            item->string = NULL;
        }
        global_hooks.deallocate(item);
        item = next;
    }
}
```

`cJSON_Delete(item->child)` recurses once per level of nesting. The C twin builds a tree 300,000 levels deep through the API and deletes it; it prints `built` and dies in the delete. A second twin prints the
same tree, with the same result (printing recurses too). I did not build the deep tree through a patch `copy`; the shape of the tree is the same however it was built.

<!-- example: 15_cjson_09_delete_deep.c -->
```c
// cJSON_Delete calls itself for every child, so a tree 300000 levels deep needs 300000 stack frames to free.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *top = cJSON_CreateArray();
    cJSON *cur = top;
    int i;
    for (i = 0; i < 300000; i++) {
        cJSON *next = cJSON_CreateArray();
        cJSON_AddItemToArray(cur, next);
        cur = next;
    }
    printf("built\n");
    fflush(stdout);
    cJSON_Delete(top);
    printf("deleted\n");
    return 0;
}
```
<!-- example: 15_cjson_09b_print_deep.c -->
```c
// The same tree as 09_delete_deep, printed instead of deleted.
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
int main(void) {
    cJSON *top = cJSON_CreateArray();
    cJSON *cur = top;
    int i;
    for (i = 0; i < 300000; i++) {
        cJSON *next = cJSON_CreateArray();
        cJSON_AddItemToArray(cur, next);
        cur = next;
    }
    printf("built\n");
    fflush(stdout);
    printf("%d\n", (int)strlen(cJSON_PrintUnformatted(top)));
    return 0;
}
```

**In cJSON-cling.** `deleteItem` is empty on purpose (the callee's handle is dropped, and the generated destructor, shown above, releases the chain iteratively when the last handle goes):

<!-- source: cjson-cling/src/cjson_item.clg "void deleteItem(Item item)" -->
```cpp
void deleteItem(Item item) {
}
```
<!-- example: 15_cjson_09_delete_deep.clg -->
```cpp
// cJSON_Delete recurses once per nesting level, so a deeply nested tree overflows the stack when freed.
// 300000 levels built through the API, then released.
#include "cjson_item.clh"
int main() {
    Item top = createArray();
    Item cur = top;
    int i = 0;
    while (i < 300000) {
        Item next = createArray();
        addItemToArray(cur, next);
        cur = next;
        i = i + 1;
    }
    std::cout << "built" << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_09_delete_deep.clg -->
```
built
```

Printing the same tree is a different function, with the same recursion as C, and it exhausts the stack:

<!-- example: 15_cjson_09b_print_deep.clg -->
```cpp
// Delete is iterative, but PRINTING a very deep tree still recurses once per level -- the same stack
// exhaustion as C, only reached through a different function. 300000 levels built through the API.
#include "cjson_item.clh"
#include "cjson_print.clh"
int main() {
    Item top = createArray();
    Item cur = top;
    int i = 0;
    while (i < 300000) {
        Item next = createArray();
        addItemToArray(cur, next);
        cur = next;
        i = i + 1;
    }
    string text = printUnformatted(top);
    std::cout << text.length() << std::endl;
    return 0;
}
```

**Verdict: partly.** The delete half is fixed by the compiler's generated destructor. The print half (and compare, and duplicate) is not.

### Issue 10: `cJSON_Compare` is exponential in nesting depth

**Defect.** `Compare` walks an object twice, a against b and then b against a, and recurses in both walks: an object nested `n` deep costs `2^n` steps.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "cJSON_ArrayForEach(a_element, a)" -->
```c
            cJSON_ArrayForEach(a_element, a)
            {
                /* TODO This has O(n^2) runtime, which is horrible! */
                b_element = get_object_item(b, a_element->string, case_sensitive);
                if (b_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }
            }
```
<!-- source: cjson-cling/upstream/cJSON.c "cJSON_ArrayForEach(b_element, b)" -->
```c
            cJSON_ArrayForEach(b_element, b)
            {
                a_element = get_object_item(a, b_element->string, case_sensitive);
                if (a_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(b_element, a_element, case_sensitive))
                {
                    return false;
                }
            }
```

Each loop calls `cJSON_Compare` on the matching member, so one level of nesting is two calls into the next. The C twin compares two chains of single-member objects, 24 deep:

<!-- example: 15_cjson_10_compare_exponential.c -->
```c
// cJSON_Compare walks an object twice, a against b and then b against a, and recurses in both walks: an object
// nested n deep costs 2^n comparisons.
#include <stdio.h>
#include "cJSON.h"
static cJSON *nest(int depth) {
    cJSON *o = cJSON_CreateObject();
    if (depth > 0) {
        cJSON_AddItemToObject(o, "a", nest(depth - 1));
    }
    return o;
}
int main(void) {
    cJSON *a = nest(24);
    cJSON *b = nest(24);
    printf("equal %d\n", (int)cJSON_Compare(a, b, 1));
    return 0;
}
```
<!-- output: 15_cjson_10_compare_exponential.c -->
```
equal 1
```

The time is not checked by the guide's tool. Measured with a throwaway driver while writing this (`gcc -O0`, one run each, this machine, `clock()` around the call only): depth 20, 0.017 s; 22, 0.065 s;
24, 0.269 s; 26, 1.09 s; 28, 4.38 s. Four times per two levels.

**In cJSON-cling.** The port has the same structure:

<!-- source: cjson-cling/src/cjson_api.clg "if (a.type == Object) {" -->
```cpp
    if (a.type == Object) {
        Item aElement = a.child;
        while (aElement != nullptr) {
            // TODO in upstream too: O(n^2)
            if (!aElement.hasKey) {
                return false;
            }
            Item bElement = getObjectItemImpl(b, aElement.key, caseSensitive);
            if (bElement == nullptr) {
                return false;
            }
            if (!compare(aElement, bElement, caseSensitive)) {
                return false;
            }
            aElement = aElement.next;
        }
        // once more the other way round, so a strict subset of b is not "equal"
        Item bElement2 = b.child;
        while (bElement2 != nullptr) {
            if (!bElement2.hasKey) {
                return false;
            }
            Item aMatch = getObjectItemImpl(a, bElement2.key, caseSensitive);
            if (aMatch == nullptr) {
                return false;
            }
            if (!compare(bElement2, aMatch, caseSensitive)) {
                return false;
            }
            bElement2 = bElement2.next;
        }
        return true;
    }
```
<!-- example: 15_cjson_10_compare_exponential.clg -->
```cpp
// Compare recurses twice per object level: {"a":{"a":...}} nested n deep costs 2^n steps.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
Item nest(int depth) {
    Item o = createObject();
    if (depth > 0) {
        addItemToObject(o, "a", nest(depth - 1));
    }
    return o;
}
int main() {
    Item a = nest(24);
    Item b = nest(24);
    std::cout << "equal " << compare(a, b, true) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_10_compare_exponential.clg -->
```
equal 1
```

The whole program took about 0.75 s on this machine at depth 24 (three runs, `-O1` build, including building the two trees), against about 0.35 s for the C twin's whole program at `-O0`. Those are two
one-off measurements, not a benchmark; the point is that the complexity is the same.

**Verdict: not prevented.** Nothing in the language bounds CPU time.

## Part 4. Undefined behaviour and shared state: issues 30, 31, 32

### Issue 30: `cJSON_SetValuestring` compares pointers from unrelated allocations

**Defect.** The overlap test compares pointers into two different allocations, which the C standard does not define, and it wrongly refuses a legitimate call: handing an item's own string back to it.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "v1_len = strlen(valuestring);" -->
```c
    v1_len = strlen(valuestring);
    v2_len = strlen(object->valuestring);

    if (v1_len <= v2_len)
    {
        /* strcpy does not handle overlapping string: [X1, X2] [Y1, Y2] => X2 < Y1 or Y2 < X1 */
        if (!( valuestring + v1_len < object->valuestring || object->valuestring + v2_len < valuestring ))
        {
            return NULL;
        }
        strcpy(object->valuestring, valuestring);
        return object->valuestring;
    }
```
<!-- example: 15_cjson_30_set_valuestring_self.c -->
```c
// cJSON_SetValuestring compares valuestring + length with object->valuestring: pointers into two different
// allocations, which the C standard does not define. It uses the result to refuse an overlapping copy, and it
// refuses the one overlap that is harmless: an item's own string handed back to it.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *s = cJSON_CreateString("hello world");
    char *result = cJSON_SetValuestring(s, s->valuestring);
    printf("self-assign %d value %s\n", result != NULL, s->valuestring);
    return 0;
}
```
<!-- output: 15_cjson_30_set_valuestring_self.c -->
```
self-assign 0 value hello world
```

The twin reproduces the wrong refusal (`self-assign 0`). The pointer comparison itself is undefined behaviour, and on a flat address space the compiler does what the numbers say; nothing here crashed.

**In cJSON-cling.** There are no pointers to compare. A string is a value, so the question "does the new text overlap the old buffer?" has no meaning, and the self-assignment simply works:

<!-- source: cjson-cling/src/cjson_api.clg "bool setValuestring" -->
```cpp
bool setValuestring(Item object, string value) {
    if (object == nullptr || object.type != String || object.isReference) {
        return false;
    }
    if (!object.hasValuestring) {
        return false;
    }
    object.valuestring = cstr(value);
    return true;
}
```
<!-- example: 15_cjson_30_set_valuestring_self.clg -->
```cpp
// SetValuestring compares pointers from unrelated allocations and refuses legitimate calls such as assigning
// an item's own string back to itself.
#include "cjson_item.clh"
#include "cjson_api.clh"
int main() {
    Item s = createString("hello world");
    bool ok = setValuestring(s, s.valuestring);
    std::cout << "self-assign " << ok << " value " << s.valuestring << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_30_set_valuestring_self.clg -->
```
self-assign 1 value hello world
```

**Verdict: cannot happen.**

### Issue 31: `cJSON_CreateNumber` casts `NaN` to `int`

**Defect.** The saturation test (`num >= INT_MAX`, `num <= INT_MIN`) is false for `NaN`, so `(int)num` is evaluated on a `NaN`, which is undefined behaviour in C.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(cJSON *) cJSON_CreateNumber" -->
```c
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;

        /* use saturation in case of overflow */
        if (num >= INT_MAX)
        {
            item->valueint = INT_MAX;
        }
        else if (num <= (double)INT_MIN)
        {
            item->valueint = INT_MIN;
        }
        else
        {
            item->valueint = (int)num;
        }
    }

    return item;
}
```
<!-- example: 15_cjson_31_nan_to_int.c -->
```c
// cJSON_CreateNumber saturates a double that is >= INT_MAX or <= INT_MIN, then falls through to (int)num. For a
// NaN both comparisons are false, so (int)NaN is evaluated, which is undefined behaviour in C. The value printed
// here, INT_MIN, is what x86-64 produces for it; the standard promises nothing, and another CPU may print 0.
#include <math.h>
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    volatile double zero = 0.0;
    cJSON *n = cJSON_CreateNumber(zero / zero);
    cJSON *big = cJSON_CreateNumber(1.0e30);
    printf("nan->%d 1e30->%d\n", n->valueint, big->valueint);
    return 0;
}
```
<!-- output: 15_cjson_31_nan_to_int.c -->
```
nan->-2147483648 1e30->2147483647
```

This output is what x86-64 with this `gcc` does (`INT_MIN`); the standard promises nothing, and another target or compiler may print something else. `cJSON_SetNumberHelper` has the same shape.

**In cJSON-cling.** CeeLing's `int(x)` is a `static_cast`, which is still undefined for `NaN` and for out-of-range values, so the language does not help here. The port avoids it the way a careful C author
would:

<!-- source: cjson-cling/src/cjson_item.clg "int saturateToInt" -->
```cpp
int saturateToInt(double number) {
    if (number != number) {
        return 0;
    }
    if (number >= 2147483647.0) {
        return 2147483647;
    }
    if (number <= -2147483648.0) {
        return -2147483647 - 1;
    }
    return int(number);
}
```
<!-- example: 15_cjson_31_nan_to_int.clg -->
```cpp
// CreateNumber/SetNumberHelper cast a NaN double to int (undefined behaviour in C).
#include "cjson_item.clh"
int main() {
    double zero = 0.0;
    Item n = createNumber(zero / zero);
    Item big = createNumber(1.0e30);
    std::cout << "nan->" << n.valueint << " 1e30->" << big.valueint << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_31_nan_to_int.clg -->
```
nan->0 1e30->2147483647
```

**Verdict: avoided by the library's own discipline, not by the language.** (The upstream bug is a missing `NaN` test; the port has the test.)

### Issue 32: `static` state shared by every caller

**Defect.** `cJSON_Version` formats into a `static` buffer, and the parser keeps its error record in one file-scope variable, so concurrent callers race on both.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "CJSON_PUBLIC(const char*) cJSON_Version" -->
```c
CJSON_PUBLIC(const char*) cJSON_Version(void)
{
    static char version[15];
    sprintf(version, "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR, CJSON_VERSION_PATCH);

    return version;
}
```
<!-- source: cjson-cling/upstream/cJSON.c "static error global_error" -->
```c
static error global_error = { NULL, 0 };

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return (const char*) (global_error.json + global_error.position);
}
```

I did **not** reproduce a data race: a race is not deterministic, and the machine is not the place to wait for one. What the C twin shows deterministically is the sharing that makes the race possible. Two
"callers" run one after the other in one thread; the second caller's successful parse resets the record the first is about to read.

<!-- example: 15_cjson_32_static_state.c -->
```c
// cJSON keeps the position of the last parse error in one file-scope variable (global_error), and cJSON_Version
// formats into one static buffer. Every caller shares them. Here two "callers" run one after the other in one
// thread: caller B's successful parse resets the record that caller A is about to read. With two threads the
// same sharing is a data race, which this program does not demonstrate.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    const char *textA = "[1,";
    const char *textB = "[2]";
    cJSON *a = cJSON_Parse(textA);
    printf("caller A: parse failed, error at %d\n", (int)(cJSON_GetErrorPtr() - textA));
    a = cJSON_Parse(textB);                       /* caller B, somewhere else in the program */
    printf("caller B: parse %s\n", a != NULL ? "succeeded" : "failed");
    printf("caller A asks again: the error pointer is NULL: %d\n", cJSON_GetErrorPtr() == NULL);
    return 0;
}
```
<!-- output: 15_cjson_32_static_state.c -->
```
caller A: parse failed, error at 3
caller B: parse succeeded
caller A asks again: the error pointer is NULL: 1
```

**In cJSON-cling.** The compiler rejects file-scope variables, so the port could not have written `global_error`:

<!-- example: 15_cjson_32_file_scope_rejected.clg -->
```cpp
// cJSON keeps the last parse error in a file-scope variable (static error global_error). CeeLing has no such
// thing: a variable declared outside every function and class is a compile error.
int lastErrorPosition = 0;

int parse(string text) {
    lastErrorPosition = text.length();
    return 0;
}

int main() {
    parse("[1,");
    std::cout << lastErrorPosition << std::endl;
    return 0;
}
```
<!-- error: 15_cjson_32_file_scope_rejected.clg -->
```
error[semantic/forbidden-construct]: statement at file scope
  --> 15_cjson_32_file_scope_rejected.clg:3:1
  help: File-scope variables are not supported (global mutable state has no owner and no lifetime the checker can reason about). Put the value in a function, or pass it as a parameter or a class field.
```

The port returns the position in each call's own result, and `version()` returns a literal:

<!-- source: cjson-cling/src/cjson_api.clg "string version" -->
```cpp
string version() {
    return "1.7.19";
}
```
<!-- example: 15_cjson_32_error_in_result.clg -->
```cpp
// The port has no saved error: the position is a field of the ParseResult each call returns, so caller B's
// parse cannot touch what caller A holds.
#include "cjson_item.clh"
#include "cjson_parse.clh"
int main() {
    ParseResult a = parseWithOptsResult("[1,", false);
    std::cout << "caller A: parse failed, error at " << a.errorPos << std::endl;
    ParseResult b = parseWithOptsResult("[2]", false);
    if (b.item != nullptr) {
        std::cout << "caller B: parse succeeded" << std::endl;
    }
    std::cout << "caller A asks again: error at " << a.errorPos << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_32_error_in_result.clg -->
```
caller A: parse failed, error at 3
caller B: parse succeeded
caller A asks again: error at 3
```

**Verdict: cannot happen**, for this library's own state. Two threads sharing one tree are outside what is checked unless they go through `spawn` and `Channel` ([chapter 10](10-threads.md)); the port's
`tests/run_threads.sh` checks something narrower, that threads each building their own trees through the pooled allocator do not corrupt each other (8 threads, no corruption, according to
`docs/performance.md`; the same test with one global free list segfaults).

## Part 5. Logic bugs: issues 14 to 29 and 33

They are grouped here by the part of the library they live in (sorting and overwriting, JSON Pointer, JSON Patch, JSON Merge Patch, text and numbers, compare, error positions), so the numbers are not in order; the full table at the end is.

None of these is a memory error; they are the bulk of the list. CeeLing does not prevent them, and the port reproduces every one *exactly*, because the differential test means nothing unless the port is
the same program. Below, each issue shows the C twin and the CeeLing program side by side; **both print the same thing**, and that is the point. The one thing the port does change is the blast
radius: where the article reports a leak (14, 24), the dropped nodes are simply released, so the same wrong answer arrives without the leak.

### The sorted list and the object that lost its siblings: issues 14 and 24

#### Issue 14: `sort_list` / `sort_object` leave the new head's tail pointer unset

**Defect.** After `cJSONUtils_SortObject` the first child's `prev` is not the last child, and `add_item_to_array` finds the end through that pointer: a later append is silently dropped.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static void sort_object" -->
```c
static void sort_object(cJSON * const object, const cJSON_bool case_sensitive)
{
    if (object == NULL)
    {
        return;
    }
    object->child = sort_list(object->child, case_sensitive);
}
```
<!-- source: cjson-cling/upstream/cJSON.c "static cJSON_bool add_item_to_array" -->
```c
static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL) || (array == item))
    {
        return false;
    }

    child = array->child;
    /*
     * To find the last item in array quickly, we use prev in array
     */
    if (child == NULL)
    {
        /* list is empty, start new one */
        array->child = item;
        item->prev = item;
        item->next = NULL;
    }
    else
    {
        /* append to the end */
        if (child->prev)
        {
            suffix_object(child->prev, item);
            array->child->prev = item;
        }
    }

    return true;
}
```

`sort_object` stores the new head and never repairs `head->prev`; `add_item_to_array` then takes `if (child->prev)` as false and does nothing, while the caller is told it worked. The twin also counts
what is still allocated after `cJSON_Delete`: the dropped member and its key are leaked.

<!-- example: 15_cjson_14_sort_then_add.c -->
```c
// After cJSONUtils_SortObject the new first child's prev is not the last child, and cJSON_AddItemToArray finds
// the end through that pointer: the new member is silently dropped while the call reports success. Nothing
// refers to the dropped member any more, so cJSON_Delete cannot free it. An allocator installed with
// cJSON_InitHooks counts the blocks that are still alive afterwards.
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "cJSON_Utils.h"

static long live_blocks = 0;

static void *counting_alloc(size_t size) {
    live_blocks++;
    return malloc(size);
}

static void counting_free(void *p) {
    if (p != NULL) live_blocks--;
    free(p);
}

int main(void) {
    cJSON_Hooks hooks = { counting_alloc, counting_free };
    cJSON *o, *added;
    char *text;
    cJSON_InitHooks(&hooks);
    o = cJSON_Parse("{\"b\":1,\"a\":2}");
    cJSONUtils_SortObject(o);
    added = cJSON_AddNumberToObject(o, "c", 3.0);
    text = cJSON_PrintUnformatted(o);
    printf("returned %d object %s\n", added != NULL, text);
    cJSON_free(text);
    cJSON_Delete(o);
    printf("blocks still allocated after cJSON_Delete: %ld\n", live_blocks);
    return 0;
}
```
<!-- output: 15_cjson_14_sort_then_add.c -->
```
returned 1 object {"a":2,"b":1}
blocks still allocated after cJSON_Delete: 2
```

**In cJSON-cling.** The same code, reproduced:

<!-- source: cjson-cling/src/cjson_item.clg "bool addItemToArray" -->
```cpp
bool addItemToArray(Item array, Item item) {
    if (item == nullptr || array == nullptr || array == item) {
        return false;
    }
    Item child = array.child;
    if (child == nullptr) {
        array.child = item;
        item.prev = item;
        item.next = nullptr;
    } else {
        if (child.prev != nullptr) {
            suffixObject(child.prev, item);
            array.child.prev = item;
        }
    }
    return true;
}
```
<!-- example: 15_cjson_14_sort_then_add.clg -->
```cpp
// After SortObject the new head's prev is not the tail, so a later append is silently dropped.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item o = parse("{\"b\":1,\"a\":2}");
    sortObject(o);
    Item added = addNumberToObject(o, "c", 3.0);
    std::cout << "returned " << (added != nullptr) << " object " << printUnformatted(o) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_14_sort_then_add.clg -->
```
returned 1 object {"a":2,"b":1}
```

**Verdict: same bug** (the member is lost); the leak is gone, because the dropped node has no owner and is released.

#### Issue 24: `overwrite_item` copies the whole struct over a non-root node

**Defect.** A root-level replace applied to a member copies the replacement struct over it with `memcpy`, wiping the member's sibling links and key.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static void overwrite_item" -->
```c
static void overwrite_item(cJSON * const root, const cJSON replacement)
{
    if (root == NULL)
    {
        return;
    }

    if (root->string != NULL)
    {
        cJSON_free(root->string);
    }
    if (root->valuestring != NULL)
    {
        cJSON_free(root->valuestring);
    }
    if (root->child != NULL)
    {
        cJSON_Delete(root->child);
    }

    memcpy(root, &replacement, sizeof(cJSON));
}
```
<!-- example: 15_cjson_24_overwrite_wipes_siblings.c -->
```c
// overwrite_item copies the whole replacement struct over the node (memcpy), so a root-level replace applied to
// a member (here "b") also overwrites the member's next and prev links and its key. "c" is cut off, "b" has
// lost its name; the nodes dropped from the list are leaked.
#include <stdio.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1,\"b\":2,\"c\":3}");
    cJSON *b = cJSON_GetObjectItem(doc, "b");
    cJSON *patches = cJSON_Parse("[{\"op\":\"replace\",\"path\":\"\",\"value\":9}]");
    int status = cJSONUtils_ApplyPatches(b, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
```
<!-- output: 15_cjson_24_overwrite_wipes_siblings.c -->
```
status 0 doc {"a":1,"":9}
```

`b` loses its key and its `next`, so `c` is cut off the list (and leaked) and `b` prints with an empty name.

**In cJSON-cling.** The port's `overwriteItem` (quoted under issue 1) copies the same fields, links included:

<!-- example: 15_cjson_24_overwrite_wipes_siblings.clg -->
```cpp
// overwrite_item copies the replacement struct over a non-root node, wiping its sibling links.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"a\":1,\"b\":2,\"c\":3}");
    Item b = getObjectItem(doc, "b");
    Item patches = parse("[{\"op\":\"replace\",\"path\":\"\",\"value\":9}]");
    int status = applyPatches(b, patches);
    std::cout << "status " << status << " doc " << printUnformatted(doc) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_24_overwrite_wipes_siblings.clg -->
```
status 0 doc {"a":1,"":9}
```

**Verdict: same bug** (leak gone).

### JSON Pointer: issues 15 to 19

#### Issue 15: `decode_pointer_inplace` decodes `~1` into the wrong position

**Defect.** The decoder walks two indices through one buffer, writes the `/` for `~1` through the wrong index, and never copies an ordinary character after an escape, so the result is not the RFC 6901 decoding.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static void decode_pointer_inplace" -->
```c
static void decode_pointer_inplace(unsigned char *string)
{
    unsigned char *decoded_string = string;

    if (string == NULL) {
        return;
    }

    for (; *string; (void)decoded_string++, string++)
    {
        if (string[0] == '~')
        {
            if (string[1] == '0')
            {
                decoded_string[0] = '~';
            }
            else if (string[1] == '1')
            {
                decoded_string[1] = '/';
            }
            else
            {
                /* invalid escape sequence */
                return;
            }

            string++;
        }
    }

    decoded_string[0] = '\0';
}
```
<!-- example: 15_cjson_15_pointer_decode_tilde1.c -->
```c
// decode_pointer_inplace writes the "/" for "~1" through the wrong index and never copies ordinary characters after
// an escape, so the key that the pointer "/a~1b" names is not "a/b".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"add\",\"path\":\"/a~1b\",\"value\":1}]");
    int status = cJSONUtils_ApplyPatches(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
```
<!-- output: 15_cjson_15_pointer_decode_tilde1.c -->
```
status 0 doc {"a~/":1}
```

The member that `/a~1b` should create is `a/b`; the twin shows it created `a~/`.

**In cJSON-cling.** The port reproduces the same walk, byte for byte, because the differential test compares against it:

<!-- source: cjson-cling/src/cjson_utilities.clg "string decodePointerInPlace" -->
```cpp
string decodePointerInPlace(string text) {
    std::vector<int> buf;
    int i = 0;
    while (i < text.length()) {
        buf.push_back(byteAt(text, i));
        i = i + 1;
    }
    buf.push_back(0);
    int d = 0;
    int s = 0;
    while (buf[s] != 0) {
        if (buf[s] == 126) {
            if (buf[s + 1] == 48) {
                buf[d] = 126;
            } else if (buf[s + 1] == 49) {
                buf[d + 1] = 47;
            } else {
                // invalid escape sequence: stop where we are
                return bufferText(buf);
            }
            s = s + 1;
        }
        d = d + 1;
        s = s + 1;
    }
    buf[d] = 0;
    return bufferText(buf);
}
```
<!-- example: 15_cjson_15_pointer_decode_tilde1.clg -->
```cpp
// decode_pointer_inplace writes the "/" for "~1" through the wrong index and never copies ordinary characters after
// an escape, so the key that the pointer "/a~1b" names is not "a/b".
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{}");
    Item patches = parse("[{\"op\":\"add\",\"path\":\"/a~1b\",\"value\":1}]");
    int status = applyPatches(doc, patches);
    std::cout << "status " << status << " doc " << printUnformatted(doc) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_15_pointer_decode_tilde1.clg -->
```
status 0 doc {"a~/":1}
```

**Verdict: same bug.**

#### Issue 16: an invalid `~` escape is ignored instead of failing

**Defect.** The last branch of the decoder returns at once on an escape other than `~0` or `~1`, without failing the patch.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "if (string[0] == '~')" -->
```c
        if (string[0] == '~')
        {
            if (string[1] == '0')
            {
                decoded_string[0] = '~';
            }
            else if (string[1] == '1')
            {
                decoded_string[1] = '/';
            }
            else
            {
                /* invalid escape sequence */
                return;
            }

            string++;
        }
```
<!-- example: 15_cjson_16_pointer_invalid_escape.c -->
```c
// An invalid "~" escape (here "~2") stops decode_pointer_inplace quietly: it neither fails the patch nor decodes the
// rest of the key.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"add\",\"path\":\"/a~2b\",\"value\":1}]");
    int status = cJSONUtils_ApplyPatches(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
```
<!-- output: 15_cjson_16_pointer_invalid_escape.c -->
```
status 0 doc {"a~2b":1}
```

The last `else` is the branch in question. The `add` succeeds with status 0 and the key is whatever text was there, `a~2b`.

**In cJSON-cling.** The same branch:

<!-- example: 15_cjson_16_pointer_invalid_escape.clg -->
```cpp
// An invalid "~" escape (here "~2") stops decode_pointer_inplace quietly: it neither fails the patch nor decodes the
// rest of the key.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{}");
    Item patches = parse("[{\"op\":\"add\",\"path\":\"/a~2b\",\"value\":1}]");
    int status = applyPatches(doc, patches);
    std::cout << "status " << status << " doc " << printUnformatted(doc) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_16_pointer_invalid_escape.clg -->
```
status 0 doc {"a~2b":1}
```

**Verdict: same bug.**

#### Issue 17: `decode_array_index_from_pointer` has no overflow check

**Defect.** The digits are accumulated into a `size_t` with no overflow check, so a huge index wraps around to a small valid one.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "for (position = 0; (pointer[position] >= '0')" -->
```c
    for (position = 0; (pointer[position] >= '0') && (pointer[position] <= '9'); position++)
    {
        parsed_index = (10 * parsed_index) + (size_t)(pointer[position] - '0');

    }
```
<!-- example: 15_cjson_17_index_wraps.c -->
```c
// decode_array_index_from_pointer adds up the digits in a size_t with no overflow check, so 2^64 wraps around to 0
// and the pointer "/18446744073709551616" names the first element.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("[10,20,30]");
    cJSON *found = cJSONUtils_GetPointer(doc, "/18446744073709551616");
    if (found == NULL) {
        printf("nothing\n");
    } else {
        printf("found %s\n", cJSON_PrintUnformatted(found));
    }
    return 0;
}
```
<!-- output: 15_cjson_17_index_wraps.c -->
```
found 10
```

`18446744073709551616` is `2^64`; it wraps to 0 and the pointer names the first element. (The wrap is unsigned arithmetic, which C defines; the bug is that nobody checked.)

**In cJSON-cling.** There is no 64-bit `size_t` to wrap, so the port emulates the wrap with two 32-bit halves to stay identical:

<!-- source: cjson-cling/src/cjson_utilities.clg "PointerIndex decodeArrayIndex" -->
```cpp
PointerIndex decodeArrayIndex(string pointer, int start) {
    PointerIndex result = new PointerIndex();
    if (byteAt(pointer, start) == 48 && byteAt(pointer, start + 1) != 0 && byteAt(pointer, start + 1) != 47) {
        return result;
    }
    int64 low = int64(0);
    int64 high = int64(0);
    int position = start;
    while (byteAt(pointer, position) >= 48 && byteAt(pointer, position) <= 57) {
        int64 next = low * int64(10) + int64(byteAt(pointer, position) - 48);
        int64 carry = next / int64(4294967296);
        low = next % int64(4294967296);
        high = (high * int64(10) + carry) % int64(4294967296);
        position = position + 1;
    }
    if (byteAt(pointer, position) != 0 && byteAt(pointer, position) != 47) {
        return result;
    }
    result.ok = true;
    result.huge = high > int64(0);
    result.value = low;
    return result;
}
```
<!-- example: 15_cjson_17_index_wraps.clg -->
```cpp
// decode_array_index_from_pointer adds up the digits in a size_t with no overflow check, so 2^64 wraps around to 0
// and the pointer "/18446744073709551616" names the first element.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("[10,20,30]");
    Item found = getPointer(doc, "/18446744073709551616");
    if (found == nullptr) {
        std::cout << "nothing" << std::endl;
    } else {
        std::cout << "found " << printUnformatted(found) << std::endl;
    }
    return 0;
}
```
<!-- output: 15_cjson_17_index_wraps.clg -->
```
found 10
```

**Verdict: same bug.** (An index of 2^32 or more that does *not* wrap back is treated as "past the end", since no list is that long; the source comment says so.)

#### Issue 18: an empty token is accepted as index 0

**Defect.** The same function has no "at least one digit" test, so an empty token (`/a/`) is index 0.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static cJSON_bool decode_array_index_from_pointer" -->
```c
static cJSON_bool decode_array_index_from_pointer(const unsigned char * const pointer, size_t * const index)
{
    size_t parsed_index = 0;
    size_t position = 0;

    if ((pointer[0] == '0') && ((pointer[1] != '\0') && (pointer[1] != '/')))
    {
        /* leading zeroes are not permitted */
        return 0;
    }

    for (position = 0; (pointer[position] >= '0') && (pointer[position] <= '9'); position++)
    {
        parsed_index = (10 * parsed_index) + (size_t)(pointer[position] - '0');

    }

    if ((pointer[position] != '\0') && (pointer[position] != '/'))
    {
        return 0;
    }

    *index = parsed_index;

    return 1;
}
```
<!-- example: 15_cjson_18_empty_index_token.c -->
```c
// An empty token after the last "/" is accepted as the index 0: the pointer "/a/" names the first element of "a".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":[10,20]}");
    cJSON *found = cJSONUtils_GetPointer(doc, "/a/");
    if (found == NULL) {
        printf("nothing\n");
    } else {
        printf("found %s\n", cJSON_PrintUnformatted(found));
    }
    return 0;
}
```
<!-- output: 15_cjson_18_empty_index_token.c -->
```
found 10
```

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_utilities.clg "int position = start;" -->
```cpp
    int position = start;
    while (byteAt(pointer, position) >= 48 && byteAt(pointer, position) <= 57) {
        int64 next = low * int64(10) + int64(byteAt(pointer, position) - 48);
        int64 carry = next / int64(4294967296);
        low = next % int64(4294967296);
        high = (high * int64(10) + carry) % int64(4294967296);
        position = position + 1;
    }
```
<!-- example: 15_cjson_18_empty_index_token.clg -->
```cpp
// An empty token after the last "/" is accepted as the index 0: the pointer "/a/" names the first element of "a".
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"a\":[10,20]}");
    Item found = getPointer(doc, "/a/");
    if (found == nullptr) {
        std::cout << "nothing" << std::endl;
    } else {
        std::cout << "found " << printUnformatted(found) << std::endl;
    }
    return 0;
}
```
<!-- output: 15_cjson_18_empty_index_token.clg -->
```
found 10
```

**Verdict: same bug.**

#### Issue 19: a pointer without a leading `/` resolves to the root

**Defect.** `get_item_from_pointer` follows tokens only while the pointer starts with `/`; any other non-empty text walks zero steps and returns the object it was given.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static cJSON *get_item_from_pointer" 16 -->
```c
static cJSON *get_item_from_pointer(cJSON * const object, const char * pointer, const cJSON_bool case_sensitive)
{
    cJSON *current_element = object;

    if (pointer == NULL)
    {
        return NULL;
    }

    /* follow path of the pointer */
    while ((pointer[0] == '/') && (current_element != NULL))
    {
        pointer++;
        if (cJSON_IsArray(current_element))
        {
            size_t index = 0;
    ...
```
<!-- example: 15_cjson_19_no_leading_slash.c -->
```c
// get_item_from_pointer follows a pointer only while it starts with "/": any other non-empty text, such as "a", walks
// zero steps and returns the root.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1}");
    cJSON *found = cJSONUtils_GetPointer(doc, "a");
    if (found == doc) {
        printf("the root\n");
    } else if (found == NULL) {
        printf("nothing\n");
    } else {
        printf("found %s\n", cJSON_PrintUnformatted(found));
    }
    return 0;
}
```
<!-- output: 15_cjson_19_no_leading_slash.c -->
```
the root
```

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_utilities.clg "Item getItemFromPointer" 8 -->
```cpp
Item getItemFromPointer(Item object, string pointerText, bool caseSensitive) {
    string pointer = cstr(pointerText);
    Item currentElement = object;
    int p = 0;
    while (byteAt(pointer, p) == 47 && currentElement != nullptr) {
        p = p + 1;
        if (isArray(currentElement)) {
            PointerIndex index = decodeArrayIndex(pointer, p);
    ...
```
<!-- example: 15_cjson_19_no_leading_slash.clg -->
```cpp
// get_item_from_pointer follows a pointer only while it starts with "/": any other non-empty text, such as "a", walks
// zero steps and returns the root.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"a\":1}");
    Item found = getPointer(doc, "a");
    if (found == doc) {
        std::cout << "the root" << std::endl;
    } else if (found == nullptr) {
        std::cout << "nothing" << std::endl;
    } else {
        std::cout << "found " << printUnformatted(found) << std::endl;
    }
    return 0;
}
```
<!-- output: 15_cjson_19_no_leading_slash.clg -->
```
the root
```

**Verdict: same bug.**

### JSON Patch: issues 20 and 23

#### Issue 20: `detach_path` mixes case sensitivity

**Defect.** The parent is found with the case flag the caller chose; the child is removed with `cJSON_DetachItemFromObject`, which ignores case.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "else if (cJSON_IsObject(parent))" -->
```c
    else if (cJSON_IsObject(parent))
    {
        detached_item = cJSON_DetachItemFromObject(parent, (char*)child_pointer);
    }
```
<!-- example: 15_cjson_20_case_flag_mismatch.c -->
```c
// detach_path finds the parent with the case flag it was given, but removes the child with
// cJSON_DetachItemFromObject, which ignores case: in the case-sensitive entry point "remove /a" removes the member "A".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"A\":1,\"b\":2}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"remove\",\"path\":\"/a\"}]");
    int status = cJSONUtils_ApplyPatchesCaseSensitive(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
```
<!-- output: 15_cjson_20_case_flag_mismatch.c -->
```
status 0 doc {"b":2}
```

In the case-sensitive entry point, `remove /a` should find nothing (the member is `A`) and fail with status 13. It removes `A` and reports success.

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_utilities.clg "Item detachPath" -->
```cpp
Item detachPath(Item object, string pathText, bool caseSensitive) {
    string path = cstr(pathText);
    int slash = lastSlash(path);
    if (slash < 0) {
        return nullptr;
    }
    string parentPointer = path.substr(0, slash);
    string childPointer = path.substr(slash + 1, path.length() - slash - 1);
    Item parent = getItemFromPointer(object, parentPointer, caseSensitive);
    childPointer = decodePointerInPlace(childPointer);
    if (isArray(parent)) {
        PointerIndex index = decodeArrayIndex(childPointer, 0);
        if (!index.ok) {
            return nullptr;
        }
        return detachItemFromArrayAt(parent, index);
    }
    if (isObject(parent)) {
        return detachItemFromObject(parent, childPointer);
    }
    return nullptr;
}
```
<!-- example: 15_cjson_20_case_flag_mismatch.clg -->
```cpp
// detach_path finds the parent with the case flag it was given, but removes the child with
// cJSON_DetachItemFromObject, which ignores case: in the case-sensitive entry point "remove /a" removes the member "A".
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"A\":1,\"b\":2}");
    Item patches = parse("[{\"op\":\"remove\",\"path\":\"/a\"}]");
    int status = applyPatchesCaseSensitive(doc, patches);
    std::cout << "status " << status << " doc " << printUnformatted(doc) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_20_case_flag_mismatch.clg -->
```
status 0 doc {"b":2}
```

**Verdict: same bug.**

#### Issue 23: `apply_patch` removes the target before validating the operation

**Defect.** For `replace`, `apply_patch` detaches and deletes the old item first and looks for `"value"` afterwards, so a replace that fails with status 7 has already destroyed the data.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "if ((opcode == REMOVE) || (opcode == REPLACE))" -->
```c
    if ((opcode == REMOVE) || (opcode == REPLACE))
    {
        /* Get rid of old. */
        cJSON *old_item = detach_path(object, (unsigned char*)path->valuestring, case_sensitive);
        if (old_item == NULL)
        {
            status = 13;
            goto cleanup;
        }
        cJSON_Delete(old_item);
        if (opcode == REMOVE)
        {
            /* For Remove, this job is done. */
            status = 0;
            goto cleanup;
        }
    }
```
<!-- source: cjson-cling/upstream/cJSON_Utils.c "Add/Replace uses" -->
```c
    else /* Add/Replace uses "value". */
    {
        value = get_object_item(patch, "value", case_sensitive);
        if (value == NULL)
        {
            /* missing "value" for add/replace. */
            status = 7;
            goto cleanup;
        }
        value = cJSON_Duplicate(value, 1);
        if (value == NULL)
        {
            /* out of memory for add/replace. */
            status = 8;
            goto cleanup;
        }
    }
```
<!-- example: 15_cjson_23_failed_replace_removes.c -->
```c
// apply_patch detaches and deletes the target of a "replace" before it looks for the "value": a replace without one
// fails with status 7 after it has already destroyed the member.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"replace\",\"path\":\"/a\"}]");
    int status = cJSONUtils_ApplyPatches(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
```
<!-- output: 15_cjson_23_failed_replace_removes.c -->
```
status 7 doc {}
```

**In cJSON-cling.** The port keeps the order, and the data is gone just the same; only the freeing is not its doing:

<!-- source: cjson-cling/src/cjson_utilities.clg "if (opcode == 2 || opcode == 3) {" -->
```cpp
    if (opcode == 2 || opcode == 3) {
        // get rid of the old one
        Item oldItem = detachPath(object, path.valuestring, caseSensitive);
        if (oldItem == nullptr) {
            return 13;
        }
        if (opcode == 2) {
            return 0;
        }
    }
```
<!-- source: cjson-cling/src/cjson_utilities.clg "// add / replace use" -->
```cpp
        // add / replace use "value"
        value = getObjectItemImpl(patch, "value", caseSensitive);
        if (value == nullptr) {
            return 7;
        }
```
<!-- example: 15_cjson_23_failed_replace_removes.clg -->
```cpp
// apply_patch detaches and deletes the target of a "replace" before it looks for the "value": a replace without one
// fails with status 7 after it has already destroyed the member.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item doc = parse("{\"a\":1}");
    Item patches = parse("[{\"op\":\"replace\",\"path\":\"/a\"}]");
    int status = applyPatches(doc, patches);
    std::cout << "status " << status << " doc " << printUnformatted(doc) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_23_failed_replace_removes.clg -->
```
status 7 doc {}
```

**Verdict: same bug.**

### JSON Merge Patch: issues 21 and 22

#### Issue 21: `generate_merge_patch` sorts with one comparator and walks with another

**Defect.** The keys are sorted with the case flag the caller chose, and then walked with plain `strcmp`. Where the two orders disagree, the walk pairs the wrong keys.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "static cJSON *generate_merge_patch" 34 -->
```c
static cJSON *generate_merge_patch(cJSON * const from, cJSON * const to, const cJSON_bool case_sensitive)
{
    cJSON *from_child = NULL;
    cJSON *to_child = NULL;
    cJSON *patch = NULL;
    if (to == NULL)
    {
        /* patch to delete everything */
        return cJSON_CreateNull();
    }
    if (!cJSON_IsObject(to) || !cJSON_IsObject(from))
    {
        return cJSON_Duplicate(to, 1);
    }

    sort_object(from, case_sensitive);
    sort_object(to, case_sensitive);

    from_child = from->child;
    to_child = to->child;
    patch = cJSON_CreateObject();
    if (patch == NULL)
    {
        return NULL;
    }
    while (from_child || to_child)
    {
        int diff;
        if (from_child != NULL)
        {
            if (to_child != NULL)
            {
                diff = strcmp(from_child->string, to_child->string);
            }
    ...
```
<!-- example: 15_cjson_21_merge_patch_case_comparator.c -->
```c
// generate_merge_patch sorts both objects with the comparator it was given (case-insensitive for GenerateMergePatch) and
// then walks them with plain strcmp. Where the two orders disagree ("a" sorts before "B", but strcmp puts "B" first) the
// walk pairs the wrong keys, and the patch both keeps and removes "B".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *from = cJSON_Parse("{\"a\":1,\"B\":1}");
    cJSON *to = cJSON_Parse("{\"B\":1}");
    cJSON *patch = cJSONUtils_GenerateMergePatch(from, to);
    if (patch == NULL) {
        printf("merge patch: none\n");
    } else {
        printf("merge patch %s\n", cJSON_PrintUnformatted(patch));
    }
    return 0;
}
```
<!-- output: 15_cjson_21_merge_patch_case_comparator.c -->
```
merge patch {"B":1,"a":null,"B":null}
```

A case-insensitive sort puts `a` before `B`; `strcmp` puts `B` (66) before `a` (97). The patch both keeps and removes `B`. (The obvious trigger, two keys that differ *only* by case, does not show a loss. This one was found by
searching small key sets for a case where applying the generated patch does not give the target document.)

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_utilities.clg "Item generateMergePatchImpl" 23 -->
```cpp
Item generateMergePatchImpl(Item from, Item to, bool caseSensitive) {
    if (to == nullptr) {
        // a patch that deletes everything
        return createNull();
    }
    if (!isObject(to) || !isObject(from)) {
        return duplicate(to, true);
    }
    sortObjectImpl(from, caseSensitive);
    sortObjectImpl(to, caseSensitive);
    Item fromChild = from.child;
    Item toChild = to.child;
    Item patch = createObject();
    while (fromChild != nullptr || toChild != nullptr) {
        int diff = 1;
        if (fromChild != nullptr) {
            if (toChild != nullptr) {
                diff = strcmpBytes(fromChild.key, toChild.key);
            } else {
                diff = -1;
            }
        }
        if (diff < 0) {
    ...
```
<!-- example: 15_cjson_21_merge_patch_case_comparator.clg -->
```cpp
// generate_merge_patch sorts both objects with the comparator it was given (case-insensitive for GenerateMergePatch) and
// then walks them with plain strcmp. Where the two orders disagree ("a" sorts before "B", but strcmp puts "B" first) the
// walk pairs the wrong keys, and the patch both keeps and removes "B".
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item from = parse("{\"a\":1,\"B\":1}");
    Item to = parse("{\"B\":1}");
    Item patch = generateMergePatch(from, to);
    if (patch == nullptr) {
        std::cout << "merge patch: none" << std::endl;
    } else {
        std::cout << "merge patch " << printUnformatted(patch) << std::endl;
    }
    return 0;
}
```
<!-- output: 15_cjson_21_merge_patch_case_comparator.clg -->
```
merge patch {"B":1,"a":null,"B":null}
```

**Verdict: same bug.**

#### Issue 22: the recursion goes through the case-insensitive wrapper

**Defect.** `generate_merge_patch` calls itself through the public `cJSONUtils_GenerateMergePatch`, so the case-sensitive entry point is case-sensitive only at the top level.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON_Utils.c "if (!compare_json(from_child, to_child, case_sensitive))" -->
```c
            if (!compare_json(from_child, to_child, case_sensitive))
            {
                /* not identical --> generate a patch */
                cJSON_AddItemToObject(patch, to_child->string, cJSONUtils_GenerateMergePatch(from_child, to_child));
            }
```
<!-- example: 15_cjson_22_merge_patch_nested_case.c -->
```c
// generate_merge_patch calls itself through the public case-insensitive wrapper, so the case-sensitive entry point is
// case-sensitive only at the top level. Two levels down, "a" and "A" count as the same key and the difference is lost.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *from = cJSON_Parse("{\"x\":{\"y\":{\"a\":1}}}");
    cJSON *to = cJSON_Parse("{\"x\":{\"y\":{\"A\":1}}}");
    cJSON *patch = cJSONUtils_GenerateMergePatchCaseSensitive(from, to);
    if (patch == NULL) {
        printf("merge patch: none\n");
    } else {
        printf("merge patch %s\n", cJSON_PrintUnformatted(patch));
    }
    return 0;
}
```
<!-- output: 15_cjson_22_merge_patch_nested_case.c -->
```
merge patch: none
```

Two levels down, `a` and `A` count as the same key, so the difference between the two documents is lost and no patch is produced at all.

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_utilities.clg "if (!compareJson(fromChild, toChild, caseSensitive)) {" -->
```cpp
            if (!compareJson(fromChild, toChild, caseSensitive)) {
                // not identical: generate a (case-insensitive, as upstream) sub-patch
                addItemToObject(patch, toChild.key, generateMergePatchImpl(fromChild, toChild, false));
            }
```
<!-- example: 15_cjson_22_merge_patch_nested_case.clg -->
```cpp
// generate_merge_patch calls itself through the public case-insensitive wrapper, so the case-sensitive entry point is
// case-sensitive only at the top level. Two levels down, "a" and "A" count as the same key and the difference is lost.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item from = parse("{\"x\":{\"y\":{\"a\":1}}}");
    Item to = parse("{\"x\":{\"y\":{\"A\":1}}}");
    Item patch = generateMergePatchCaseSensitive(from, to);
    if (patch == nullptr) {
        std::cout << "merge patch: none" << std::endl;
    } else {
        std::cout << "merge patch " << printUnformatted(patch) << std::endl;
    }
    return 0;
}
```
<!-- output: 15_cjson_22_merge_patch_nested_case.clg -->
```
merge patch: none
```

**Verdict: same bug** (the port passes `false` there on purpose, as upstream does).

### Minify, numbers and strings: issues 25 to 28

#### Issue 25: `cJSON_Minify` ignores backslash parity

**Defect.** Inside a string literal, any backslash followed by a quote is taken as an escaped quote, without asking whether that backslash is itself escaped.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "static void minify_string" -->
```c
static void minify_string(char **input, char **output) {
    (*output)[0] = (*input)[0];
    *input += static_strlen("\"");
    *output += static_strlen("\"");


    for (; (*input)[0] != '\0'; (void)++(*input), ++(*output)) {
        (*output)[0] = (*input)[0];

        if ((*input)[0] == '\"') {
            (*output)[0] = '\"';
            *input += static_strlen("\"");
            *output += static_strlen("\"");
            return;
        } else if (((*input)[0] == '\\') && ((*input)[1] == '\"')) {
            (*output)[1] = (*input)[1];
            *input += static_strlen("\"");
            *output += static_strlen("\"");
        }
    }
}
```
<!-- example: 15_cjson_25_minify_backslash_parity.c -->
```c
// Minify copies a string literal character by character and treats a backslash followed by a quote as an escaped quote,
// without asking whether that backslash is itself escaped. The string "x\\" (an escaped backslash, then the real closing
// quote) therefore swallows its closing quote, and the rest of the text is taken for string content.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    char buffer[] = "{\"a\":\"x\\\\\",\"b\": 1 }";
    cJSON_Minify(buffer);
    printf("minified %s\n", buffer);
    return 0;
}
```
<!-- output: 15_cjson_25_minify_backslash_parity.c -->
```
minified {"a":"x\\","b": 1 }
```

The string `"x\\"` (an escaped backslash, then the real closing quote) swallows its closing quote; the rest of the text is treated as string content, so the space in `"b": 1 }` survives.

**In cJSON-cling.** `minify` returns the minified text instead of working in place, and keeps the rule:

<!-- source: cjson-cling/src/cjson_api.clg "} else if (c == 34) {" -->
```cpp
        } else if (c == 34) {
            // string: copied verbatim up to the closing quote; only a backslash directly
            // before a quote is understood, exactly like the C version
            out.append(1, char(c));
            i = i + 1;
            while (byteAt(json, i) != 0) {
                int d = byteAt(json, i);
                out.append(1, char(d));
                if (d == 34) {
                    i = i + 1;
                    break;
                } else if (d == 92 && byteAt(json, i + 1) == 34) {
                    out.append(1, char(34));
                    i = i + 1;
                }
                i = i + 1;
            }
        } else {
            out.append(1, char(c));
            i = i + 1;
        }
```
<!-- example: 15_cjson_25_minify_backslash_parity.clg -->
```cpp
// Minify copies a string literal character by character and treats a backslash followed by a quote as an escaped quote,
// without asking whether that backslash is itself escaped. The string "x\\" (an escaped backslash, then the real closing
// quote) therefore swallows its closing quote, and the rest of the text is taken for string content.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    string minified = minify("{\"a\":\"x\\\\\",\"b\": 1 }");
    std::cout << "minified " << minified << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_25_minify_backslash_parity.clg -->
```
minified {"a":"x\\","b": 1 }
```

**Verdict: same bug.**

#### Issue 26: `print_number` / `compare_double` accept imprecise 15-digit output

**Defect.** `print_number` tries 15 significant digits and keeps them if the value read back is within a relative `DBL_EPSILON` of the original. That test accepts output that is not the number.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "/* Try 15 decimal places of precision to avoid nonsignificant nonzero digits */" -->
```c
        /* Try 15 decimal places of precision to avoid nonsignificant nonzero digits */
        length = sprintf((char*)number_buffer, "%1.15g", d);

        /* Check whether the original double can be recovered */
        if ((sscanf((char*)number_buffer, "%lg", &test) != 1) || !compare_double((double)test, d))
        {
            /* If not, print with 17 decimal places of precision */
            length = sprintf((char*)number_buffer, "%1.17g", d);
        }
```
<!-- source: cjson-cling/upstream/cJSON.c "static cJSON_bool compare_double" -->
```c
static cJSON_bool compare_double(double a, double b)
{
    double maxVal = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return (fabs(a - b) <= maxVal * DBL_EPSILON);
}
```
<!-- example: 15_cjson_26_print_number_digits.c -->
```c
// print_number prints 15 significant digits and keeps them if reading them back gives a value within DBL_EPSILON
// (relative) of the original. That test accepts output that is not the number: 0.30000000000000004 prints as 0.3.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *item = cJSON_Parse("[0.30000000000000004,123456789.123456789,1.0000000000000002]");
    printf("printed %s\n", cJSON_PrintUnformatted(item));
    return 0;
}
```
<!-- output: 15_cjson_26_print_number_digits.c -->
```
printed [0.3,123456789.12345679,1]
```

`0.30000000000000004` prints as `0.3` and `1.0000000000000002` as `1`: both are within one `DBL_EPSILON` of the original, neither is the original. `123456789.123456789` does fail the test and gets 17 digits.

**In cJSON-cling.** The same two-step print, through the shim's `printf("%g")`:

<!-- source: cjson-cling/src/cjson_print.clg "bool printNumber" -->
```cpp
    bool printNumber(Item item) {
        double d = item.valuedouble;
        string text = "";
        bool isNan = d != d;
        double diff = d - d;
        bool isInf = diff != diff && d == d;
        if (isNan || isInf) {
            text = "null";
        } else if (d == double(item.valueint)) {
            text = intToText(item.valueint);
        } else {
            // 15 significant digits are tried first; 17 only when that does not round-trip
            cjson_fmt_g_into(d, 15, text);
            int consumed = 0;
            double test = cjson_strtod_at(text, 0, text.length(), consumed);
            if (consumed == 0 || !compareDouble(test, d)) {
                cjson_fmt_g_into(d, 17, text);
            }
        }
        // upstream's 26-byte scratch buffer
        if (text.length() > 25) {
            return false;
        }
        if (!ensure(text.length() + 1)) {
            return false;
        }
        out.append(text);
        return true;
    }
```
<!-- example: 15_cjson_26_print_number_digits.clg -->
```cpp
// print_number prints 15 significant digits and keeps them if reading them back gives a value within DBL_EPSILON
// (relative) of the original. That test accepts output that is not the number: 0.30000000000000004 prints as 0.3.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item item = parse("[0.30000000000000004,123456789.123456789,1.0000000000000002]");
    std::cout << "printed " << printUnformatted(item) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_26_print_number_digits.clg -->
```
printed [0.3,123456789.12345679,1]
```

**Verdict: same bug.**

#### Issue 27: `parse_hex4` returns 0 for an invalid digit

**Defect.** An invalid hexadecimal digit makes `parse_hex4` return 0, which is also the value of a real `\u0000`, so a bad `\u` escape is accepted.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "else /* invalid */" -->
```c
        else /* invalid */
        {
            return 0;
        }
```
<!-- example: 15_cjson_27_hex4_invalid_digit.c -->
```c
// parse_hex4 returns 0 for an invalid digit, which is also the value of a real "\u0000". The escape \u12G4 is accepted
// (as the character U+0000) and the string is cut there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *item = cJSON_Parse("[\"ab\\u12G4cd\"]");
    printf("printed %s\n", cJSON_PrintUnformatted(item));
    return 0;
}
```
<!-- output: 15_cjson_27_hex4_invalid_digit.c -->
```
printed ["ab"]
```

The escape `\u12G4` is read as U+0000, and the string is cut at that NUL byte: `ab`.

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_parse.clg "int parseHex4" -->
```cpp
    int parseHex4(int position) {
        int h = 0;
        int i = 0;
        while (i < 4) {
            int c = absByte(position + i);
            if (c >= 48 && c <= 57) {
                h = h + (c - 48);
            } else if (c >= 65 && c <= 70) {
                h = h + (10 + c - 65);
            } else if (c >= 97 && c <= 102) {
                h = h + (10 + c - 97);
            } else {
                return 0;
            }
            if (i < 3) {
                h = h << 4;
            }
            i = i + 1;
        }
        return h;
    }
```
<!-- example: 15_cjson_27_hex4_invalid_digit.clg -->
```cpp
// parse_hex4 returns 0 for an invalid digit, which is also the value of a real "\u0000". The escape \u12G4 is accepted
// (as the character U+0000) and the string is cut there.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item item = parse("[\"ab\\u12G4cd\"]");
    std::cout << "printed " << printUnformatted(item) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_27_hex4_invalid_digit.clg -->
```
printed ["ab"]
```

**Verdict: same bug.**

#### Issue 28: `parse_number` has no overflow or underflow check

**Defect.** `strtod`'s range error is not looked at: `1e999` becomes infinity (which prints as `null`) and `1e-999` becomes 0.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "number = strtod(" -->
```c
    number = strtod((const char*)number_c_string, (char**)&after_end);
    if (number_c_string == after_end)
    {
        /* free the temporary buffer */
        input_buffer->hooks.deallocate(number_c_string);
        return false; /* parse_error */
    }
```
<!-- example: 15_cjson_28_number_out_of_range.c -->
```c
// parse_number does not look at the range error strtod reports: 1e999 becomes infinity (printed as null) and 1e-999
// becomes 0.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *item = cJSON_Parse("[1e999,1e-999,-1e999]");
    printf("printed %s\n", cJSON_PrintUnformatted(item));
    return 0;
}
```
<!-- output: 15_cjson_28_number_out_of_range.c -->
```
printed [null,0,null]
```

**In cJSON-cling.** The conversion is the shim's `strtod`, and the only check is the same "nothing was parsed":

<!-- source: cjson-cling/src/cjson_parse.clg "bool parseNumber" -->
```cpp
    bool parseNumber(Item item) {
        int numberLength = 0;
        int i = 0;
        while (canAccess(i)) {
            int c = byteAt(i);
            bool numeric = (c >= 48 && c <= 57) || c == 43 || c == 45 || c == 101 || c == 69 || c == 46;
            if (!numeric) {
                break;
            }
            numberLength = numberLength + 1;
            i = i + 1;
        }
        int consumed = 0;
        double number = cjson_strtod_at(content, offset, numberLength, consumed);
        if (consumed == 0) {
            // nothing parsed: upstream's number_c_string == after_end
            return false;
        }
        item.valuedouble = number;
        item.valueint = saturateToInt(number);
        item.type = Number;
        offset = offset + consumed;
        return true;
    }
```
<!-- example: 15_cjson_28_number_out_of_range.clg -->
```cpp
// parse_number does not look at the range error strtod reports: 1e999 becomes infinity (printed as null) and 1e-999
// becomes 0.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
int main() {
    Item item = parse("[1e999,1e-999,-1e999]");
    std::cout << "printed " << printUnformatted(item) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_28_number_out_of_range.clg -->
```
printed [null,0,null]
```

**Verdict: same bug.**

### Compare: issue 29

#### Issue 29: `cJSON_Compare` and duplicate keys, and infinities

**Defect.** Every key of one object is looked up in the other with `get_object_item`, which always returns the *first* match, so two identical objects that contain a duplicate key compare unequal.
Infinities compare unequal too, because `compare_double` computes `inf - inf`, which is `NaN`.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "current_element = object->child;" -->
```c
    current_element = object->child;
    if (case_sensitive)
    {
        while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;
        }
    }
```
<!-- example: 15_cjson_29_compare_duplicates_infinity.c -->
```c
// cJSON_Compare looks every key of one object up in the other with get_object_item, which returns the FIRST match, so two
// identical objects that contain a duplicate key compare unequal. Two infinities compare unequal too: compare_double
// computes inf - inf, which is NaN.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *a = cJSON_Parse("{\"a\":1,\"a\":2}");
    cJSON *b = cJSON_Parse("{\"a\":1,\"a\":2}");
    cJSON *x, *y;
    printf("duplicate keys: %s\n", cJSON_Compare(a, b, 1) ? "equal" : "NOT equal");
    x = cJSON_Parse("[1e999]");
    y = cJSON_Parse("[1e999]");
    printf("infinities: %s\n", cJSON_Compare(x, y, 1) ? "equal" : "NOT equal");
    return 0;
}
```
<!-- output: 15_cjson_29_compare_duplicates_infinity.c -->
```
duplicate keys: NOT equal
infinities: NOT equal
```

(The object walk in `cJSON_Compare` is quoted under issue 10; `compare_double` under issue 26.)

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_item.clg "bool compareDouble" -->
```cpp
bool compareDouble(double a, double b) {
    double maxVal = absDouble(a);
    if (absDouble(b) > maxVal) {
        maxVal = absDouble(b);
    }
    return absDouble(a - b) <= maxVal * (1.0 / 4503599627370496.0);
}
```
<!-- example: 15_cjson_29_compare_duplicates_infinity.clg -->
```cpp
// cJSON_Compare looks every key of one object up in the other with get_object_item, which returns the FIRST match, so two
// identical objects that contain a duplicate key compare unequal. Two infinities compare unequal too: compare_double
// computes inf - inf, which is NaN.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
string verdict(bool equal) {
    if (equal) {
        return "equal";
    }
    return "NOT equal";
}

int main() {
    Item a = parse("{\"a\":1,\"a\":2}");
    Item b = parse("{\"a\":1,\"a\":2}");
    std::cout << "duplicate keys: " << verdict(compare(a, b, true)) << std::endl;
    Item x = parse("[1e999]");
    Item y = parse("[1e999]");
    std::cout << "infinities: " << verdict(compare(x, y, true)) << std::endl;
    return 0;
}
```
<!-- output: 15_cjson_29_compare_duplicates_infinity.clg -->
```
duplicate keys: NOT equal
infinities: NOT equal
```

**Verdict: same bug.**

### Error positions: issue 33

#### Issue 33: `parse_string` advances before it validates

**Defect.** `parse_string` moves past the opening quote before it checks that there is one, so the position it reports for a malformed key or value is one byte too far.

**In cJSON.**

<!-- source: cjson-cling/upstream/cJSON.c "static cJSON_bool parse_string" 12 -->
```c
static cJSON_bool parse_string(cJSON * const item, parse_buffer * const input_buffer)
{
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;
    unsigned char *output_pointer = NULL;
    unsigned char *output = NULL;

    /* not a string */
    if (buffer_at_offset(input_buffer)[0] != '\"')
    {
        goto fail;
    }
    ...
```
<!-- source: cjson-cling/upstream/cJSON.c "if (input_pointer != NULL)" -->
```c
    if (input_pointer != NULL)
    {
        input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
    }
```
<!-- example: 15_cjson_33_error_position_off_by_one.c -->
```c
// parse_string moves past the opening quote before it checks that there is one, so the position it reports for a
// malformed key or value is one byte too far.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
static void show(const char *text) {
    const char *end = NULL;
    cJSON *item = cJSON_ParseWithOpts(text, &end, 0);
    if (item != NULL) {
        printf("%s: parsed\n", text);
    } else {
        printf("%s: error at %d\n", text, (int)(end - text));
    }
}

int main(void) {
    show("{a:1}");
    show("[1,x]");
    return 0;
}
```
<!-- output: 15_cjson_33_error_position_off_by_one.c -->
```
{a:1}: error at 2
[1,x]: error at 3
```

For `{a:1}` the bad key starts at byte 1; upstream reports 2.

**In cJSON-cling.**

<!-- source: cjson-cling/src/cjson_parse.clg "bool failString" -->
```cpp
    bool failString(int inputPointer) {
        offset = inputPointer;
        return false;
    }
```
<!-- example: 15_cjson_33_error_position_off_by_one.clg -->
```cpp
// parse_string moves past the opening quote before it checks that there is one, so the position it reports for a
// malformed key or value is one byte too far.
#include "cjson_item.clh"
#include "cjson_api.clh"
#include "cjson_parse.clh"
#include "cjson_print.clh"
#include "cjson_utilities.clh"
void show(string text) {
    ParseResult r = parseWithOptsResult(text, false);
    if (r.item != nullptr) {
        std::cout << text << ": parsed" << std::endl;
    } else {
        std::cout << text << ": error at " << r.errorPos << std::endl;
    }
}

int main() {
    show("{a:1}");
    show("[1,x]");
    return 0;
}
```
<!-- output: 15_cjson_33_error_position_off_by_one.clg -->
```
{a:1}: error at 2
[1,x]: error at 3
```

The port's `parseString` starts with `int inputPointer = offset + 1;` as upstream does, and `failString(inputPointer)` stores that as the offset.

**Verdict: same bug.**

## The full table

Examples are in `examples/`; "port" is the CeeLing program, "C" the twin.

| # | cJSON function | Defect in C | In the port | Port / C |
|---|---|---|---|---|
| 1 | `overwrite_item` | frees fields ignoring reference flags | cannot happen | [port](examples/15_cjson_01_overwrite_reference.clg) / [C](examples/15_cjson_01_overwrite_reference.c) |
| 2 | `ApplyPatches` | reads the next patch after deleting it | cannot happen | [port](examples/15_cjson_02_patch_deletes_itself.clg) / [C](examples/15_cjson_02_patch_deletes_itself.c) |
| 3 | `merge_patch` | walks deleted nodes (aliasing) | cannot happen | [port](examples/15_cjson_03_merge_patch_alias.clg) / [C](examples/15_cjson_03_merge_patch_alias.c) |
| 4 | `ReplaceItemViaPointer` | frees a referenced item; a node can replace its parent | cannot happen; the cycle is refused by library code | [port](examples/15_cjson_04_replace_with_ancestor.clg) / [C](examples/15_cjson_04_replace_with_ancestor.c) |
| 5 | `DetachItemViaPointer` | write through `NULL` `parent->child` | stopped at run time | [port](examples/15_cjson_05_detach_null_child.clg) / [C](examples/15_cjson_05_detach_null_child.c) |
| 6 | `ReplaceItemInArray` | array inside itself | partly: direct form refused by library code, indirect cycle crashes | [port](examples/15_cjson_06_array_into_itself.clg), [06b](examples/15_cjson_06b_cycle_through_two_arrays.clg) / [C](examples/15_cjson_06_array_into_itself.c), [06b](examples/15_cjson_06b_cycle_through_two_arrays.c) |
| 7 | `ParseWithOpts`, `GetErrorPtr` | stale error pointer | cannot happen | [port](examples/15_cjson_07_stale_error_pointer.clg) / [C](examples/15_cjson_07_stale_error_pointer.c) |
| 8 | `Duplicate` | cap too high for the stack | partly (unchanged): stack-dependent, no check | [port](examples/15_cjson_08_duplicate_deep.clg), [08b](examples/15_cjson_08b_duplicate_limit.clg) / [C](examples/15_cjson_08_duplicate_deep.c), [08b](examples/15_cjson_08b_duplicate_limit.c) |
| 9 | `Delete`, patch `copy` | unbounded recursion | partly: delete fixed, print still recurses | [port](examples/15_cjson_09_delete_deep.clg), [09b](examples/15_cjson_09b_print_deep.clg), [destructor](examples/15_cjson_09_drain_destructor.clg) / [C](examples/15_cjson_09_delete_deep.c), [09b](examples/15_cjson_09b_print_deep.c) |
| 10 | `Compare` | exponential time | not prevented | [port](examples/15_cjson_10_compare_exponential.clg) / [C](examples/15_cjson_10_compare_exponential.c) |
| 11 | `apply_patch` | `NULL` object dereference | stopped at run time | [port](examples/15_cjson_11_patch_null_root.clg) / [C](examples/15_cjson_11_patch_null_root.c) |
| 12 | `apply_patch`, `detach_path` | string node with a `NULL` value | cannot happen (as a crash); reads as an empty path | [port](examples/15_cjson_12_string_without_value.clg) / [C](examples/15_cjson_12_string_without_value.c) |
| 13 | `cJSON_Utils` allocations | unchecked `malloc` | cannot happen (as an unchecked write) | [port](examples/15_cjson_13_alloc_unchecked.clg) / [C](examples/15_cjson_13_alloc_unchecked.c) |
| 14 | `sort_list` | later append dropped | same bug (leak gone) | [port](examples/15_cjson_14_sort_then_add.clg) / [C](examples/15_cjson_14_sort_then_add.c) |
| 15 | `decode_pointer_inplace` | `~1` in the wrong position | same bug | [port](examples/15_cjson_15_pointer_decode_tilde1.clg) / [C](examples/15_cjson_15_pointer_decode_tilde1.c) |
| 16 | `decode_pointer_inplace` | invalid escape ignored | same bug | [port](examples/15_cjson_16_pointer_invalid_escape.clg) / [C](examples/15_cjson_16_pointer_invalid_escape.c) |
| 17 | `decode_array_index_from_pointer` | index wraps | same bug | [port](examples/15_cjson_17_index_wraps.clg) / [C](examples/15_cjson_17_index_wraps.c) |
| 18 | `decode_array_index_from_pointer` | empty token is 0 | same bug | [port](examples/15_cjson_18_empty_index_token.clg) / [C](examples/15_cjson_18_empty_index_token.c) |
| 19 | `get_item_from_pointer` | no leading `/` gives the root | same bug | [port](examples/15_cjson_19_no_leading_slash.clg) / [C](examples/15_cjson_19_no_leading_slash.c) |
| 20 | `detach_path` | mixed case sensitivity | same bug | [port](examples/15_cjson_20_case_flag_mismatch.clg) / [C](examples/15_cjson_20_case_flag_mismatch.c) |
| 21 | `generate_merge_patch` | sort and walk disagree | same bug | [port](examples/15_cjson_21_merge_patch_case_comparator.clg) / [C](examples/15_cjson_21_merge_patch_case_comparator.c) |
| 22 | `generate_merge_patch` | case-insensitive wrapper in the recursion | same bug | [port](examples/15_cjson_22_merge_patch_nested_case.clg) / [C](examples/15_cjson_22_merge_patch_nested_case.c) |
| 23 | `apply_patch` | destroys before validating | same bug | [port](examples/15_cjson_23_failed_replace_removes.clg) / [C](examples/15_cjson_23_failed_replace_removes.c) |
| 24 | `overwrite_item` | wipes sibling links | same bug (leak gone) | [port](examples/15_cjson_24_overwrite_wipes_siblings.clg) / [C](examples/15_cjson_24_overwrite_wipes_siblings.c) |
| 25 | `Minify` | backslash parity | same bug | [port](examples/15_cjson_25_minify_backslash_parity.clg) / [C](examples/15_cjson_25_minify_backslash_parity.c) |
| 26 | `print_number`, `compare_double` | imprecise 15 digits | same bug | [port](examples/15_cjson_26_print_number_digits.clg) / [C](examples/15_cjson_26_print_number_digits.c) |
| 27 | `parse_hex4` | invalid digit is 0 | same bug | [port](examples/15_cjson_27_hex4_invalid_digit.clg) / [C](examples/15_cjson_27_hex4_invalid_digit.c) |
| 28 | `parse_number` | overflow unchecked | same bug | [port](examples/15_cjson_28_number_out_of_range.clg) / [C](examples/15_cjson_28_number_out_of_range.c) |
| 29 | `Compare` | duplicate keys, infinities | same bug | [port](examples/15_cjson_29_compare_duplicates_infinity.clg) / [C](examples/15_cjson_29_compare_duplicates_infinity.c) |
| 30 | `SetValuestring` | cross-allocation pointer compare | cannot happen | [port](examples/15_cjson_30_set_valuestring_self.clg) / [C](examples/15_cjson_30_set_valuestring_self.c) |
| 31 | `CreateNumber` | `NaN` to `int` | avoided by the library (`saturateToInt`) | [port](examples/15_cjson_31_nan_to_int.clg) / [C](examples/15_cjson_31_nan_to_int.c) |
| 32 | `Version`, `global_error` | shared `static` state | cannot happen (no file-scope variables) | [rejected](examples/15_cjson_32_file_scope_rejected.clg), [port](examples/15_cjson_32_error_in_result.clg) / [C](examples/15_cjson_32_static_state.c) |
| 33 | `parse_string` | off-by-one error position | same bug | [port](examples/15_cjson_33_error_position_off_by_one.clg) / [C](examples/15_cjson_33_error_position_off_by_one.c) |

## What the exercise found in the compiler

The case study was also a test of CeeLing, and it failed that test several times. Each defect below was found by porting cJSON (or reviewing the port) and fixed with a regression fixture in
`tests/safety/`. The commit column is the CeeLing repository's commit whose subject line matches; I checked the subjects with `git log`, not the diffs.

| Found while | The problem | The fix | Commit |
|---|---|---|---|
| reviewing the first port | a `ref` bound to a field, a getter result or a call temporary lowered to a raw pointer with no tracked owner; an unknown call inside `try` bypassed the default-reject rule | `ref` may bind only to a local, a container element or another `ref`; `try`/`catch` bodies are checked | `fa366fe` |
| using `break` and `continue` | a use after a move carried across loop iterations was accepted and the generated C++ crashed | loop bodies are re-analysed from the loop-head state | `f561a3b` |
| building a long list | the generated destructor recursed once per node and overflowed the stack | an iterative destructor for self-owning classes | `d3de161` |
| writing library functions | a parameter only compared with `nullptr` was treated as consumed; a parameter stored into a field was not shared in free functions or nested blocks | `==` and `!=` on a parameter are reads; the field-store promotion covers those cases | `23b8dae` |
| reusing a local name | pointer kinds were looked up by variable name across the whole program | scope-aware lookup; locals stored into shared fields are shared | `2298576` |
| walking a list | `last = node; ... node.x` was rejected: assigning a local moved it | Java-style local aliasing; hints reach fields only (a `//@ptr weak` hint had leaked onto a parameter named `prev`) | `a43e451` |
| reading a weak link through an included class | the null-check helper was only emitted into files that declare a weak field | emitted whenever one is in scope | `346a555` |
| writing the triggers | `double x = 1.0e30;` **compiled as `1.0`**: error recovery silently dropped `e30` | any syntax error is a compile error; exponent literals exist; a compiler crash no longer counts as a rejection in the safety runner | `340ff5d` |
| the null issues | only a `weak_ptr` was checked; an empty `shared_ptr` was an ordinary C++ null dereference | a run-time check on every object-handle dereference | `3742148` |
| file-scope variables | `int counter = 5;` outside a function was silently dropped from the generated C++ | executable statements at file scope are rejected | `739a689` |
| measuring speed | every `string` argument was copied on every call | read-only string parameters pass as `const std::string&` | `77ab3f0` |
| measuring speed | one `malloc` per node made the parser 1.6 times slower than C | an opt-in per-thread pooled allocator (`--pool-shared`) | `cf869af` |

The `1.0e30` row is the one to remember: a soundness bug, in which the compiler accepted a malformed program and changed its meaning without a word, was found because a vulnerability trigger needed a large
constant. The attribution of each row to the port comes from the earlier version of this chapter; a commit subject says so only for `fa366fe` ("found reviewing the cJSON port") and, for `23b8dae`, in its body ("found while starting the cJSON port").

## Speed and memory

Safe is not free. `cJSON-cling/docs/performance.md` (gcc 16.2 `-O2`, bounds and null checks ON, built with `--pool-shared`, a 20 MB document of small records, best of 3) reports:

| | upstream cJSON | cJSON-cling |
|---|---|---|
| parse | 81 MB/s | 86 MB/s |
| print | 69 MB/s | 105 MB/s |
| free the tree | 168 MB/s | 947 MB/s |
| memory held by the tree | 207 MB | 380 MB (1.8 times) |

Every kind of token is at parity or faster except very long strings (6 percent behind on parse). The price is memory: a reference-counted node with a weak back link and two inline strings is larger than C's 64
bytes. Switching both checks off changes the speed by about 5 percent on this document (`docs/performance.md`). The first port was 1.6 times slower than C; the cost was the node, and a per-thread pooled allocator
closed it. These figures are the project's, not re-measured here, and they are one program on one machine.

## What this shows, and what it does not

**It shows**

* Of the cJSON bugs that are memory errors or crashes (1 to 13), CeeLing removes 7 outright, turns 2 from silent corruption into a located abort, leaves 3 partly open and 1 open. The mechanism is almost always
  the same: the dangerous operation is not in the language.
* It does so without lifetime annotations. The port's one explicit hint is the weak back link.
* A real library is a harder test than the fixtures. It found compiler defects and performance gaps, one of them a silent miscompilation.

**It does not show**

* That CeeLing makes code correct. 17 of the 33 are logic errors and all 17 remain. The port even adds one: in issue 12 a malformed node reads as an empty path instead of crashing.
* That it bounds resource use. Stack depth and CPU time are not checked (6b, 8, 9b, 10).
* That the hand-written parts are safe: the 52-line number shim and the C++ standard library are trusted.
* Anything about thread safety beyond [chapter 10](10-threads.md). Issue 32 is "cannot happen" for the library's own state only.
* Anything about the author's exact exploits, which were not used.
* That the port is free of every memory error. See the sanitizer caveat above.
* That upstream's behaviour is what these twins print on every platform. The twins that rely on undefined behaviour (1, 2, 3, 4, 31) say so; the rest print what C defines.

## Reproducing it

The guide's own check builds every program in this chapter against the sibling project and compares what it prints with what is shown above:

```
python3 -I docs/language-guide/examples/check.py check 15_ --chapters 15-
```

The project's own scripts, from the `cJSON-cling` folder, are the larger runs behind the figures:

```
bash tests/run_vulns.sh                     # the 20 memory-class programs (outcomes self-checked) and the logic-class driver
bash tests/run_ported.sh                    # 776 ported upstream unit-test checks
bash tests/run_diff.sh --fuzz 3000          # differential parses against upstream
bash tests/run_diff_utils.sh --fuzz 1000    # Utils: pointer, patch, merge patch, sort
bash tests/run_threads.sh                   # threads on the pooled allocator
bash tests/run_sanitizers.sh --fuzz 1500    # all of the above under ASan and UBSan, in WSL
bash tests/run_bench.sh 20                  # speed and memory against upstream
```

I did not run these scripts for this chapter. The issue list is Joshua Rogers, *33 Vulnerabilities in cJSON*, July 2026.
