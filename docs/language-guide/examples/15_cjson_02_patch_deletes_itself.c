//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: applying
//@ crash: ApplyPatches walks on into the freed patch array and follows the 0xDD fill
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
