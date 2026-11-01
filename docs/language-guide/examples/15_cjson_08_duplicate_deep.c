//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: duplicated 1
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
