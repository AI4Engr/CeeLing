//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: returned 1 object {"a":2,"b":1}
//@ run: blocks still allocated after cJSON_Delete: 2
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
