//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: printing recurses until the stack is gone
// The same cycle one step further away: a contains b, b contains a. Nothing in the API refuses it.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *a = cJSON_CreateArray();
    cJSON *b = cJSON_CreateArray();
    cJSON_AddItemToArray(a, b);
    cJSON_AddItemToArray(b, a);
    printf("built\n");
    fflush(stdout);
    printf("printed %s\n", cJSON_PrintUnformatted(a));
    return 0;
}
