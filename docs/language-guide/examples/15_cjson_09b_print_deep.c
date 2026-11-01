//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: printing recurses once per nesting level
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
