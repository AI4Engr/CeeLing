//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: duplicate of 10001 levels: refused
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
