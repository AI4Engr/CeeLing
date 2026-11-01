//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: merging
//@ crash: merge_patch deletes a node and then duplicates it
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
