//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: cJSON_Delete recurses once per nesting level
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
