//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: equal 1
// cJSON_Compare walks an object twice, a against b and then b against a, and recurses in both walks: an object
// nested n deep costs 2^n comparisons.
#include <stdio.h>
#include "cJSON.h"
static cJSON *nest(int depth) {
    cJSON *o = cJSON_CreateObject();
    if (depth > 0) {
        cJSON_AddItemToObject(o, "a", nest(depth - 1));
    }
    return o;
}
int main(void) {
    cJSON *a = nest(24);
    cJSON *b = nest(24);
    printf("equal %d\n", (int)cJSON_Compare(a, b, 1));
    return 0;
}
