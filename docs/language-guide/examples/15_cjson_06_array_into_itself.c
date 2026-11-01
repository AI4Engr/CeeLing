//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: the array now contains itself; printing it recurses until the stack is gone
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
