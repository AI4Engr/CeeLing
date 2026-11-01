//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: status 0
//@ run: first child of the original: type byte 0xDD (a live number has 0x08)
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
