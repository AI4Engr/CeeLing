//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: self-assign 0 value hello world
// cJSON_SetValuestring compares valuestring + length with object->valuestring: pointers into two different
// allocations, which the C standard does not define. It uses the result to refuse an overlapping copy, and it
// refuses the one overlap that is harmless: an item's own string handed back to it.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *s = cJSON_CreateString("hello world");
    char *result = cJSON_SetValuestring(s, s->valuestring);
    printf("self-assign %d value %s\n", result != NULL, s->valuestring);
    return 0;
}
