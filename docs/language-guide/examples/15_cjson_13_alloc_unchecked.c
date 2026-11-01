//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: looking for the pointer
//@ crash: sprintf into the NULL that cJSON_malloc returned
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
