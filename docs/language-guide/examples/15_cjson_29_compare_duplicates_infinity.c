//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: duplicate keys: NOT equal
//@ run: infinities: NOT equal
// cJSON_Compare looks every key of one object up in the other with get_object_item, which returns the FIRST match, so two
// identical objects that contain a duplicate key compare unequal. Two infinities compare unequal too: compare_double
// computes inf - inf, which is NaN.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *a = cJSON_Parse("{\"a\":1,\"a\":2}");
    cJSON *b = cJSON_Parse("{\"a\":1,\"a\":2}");
    cJSON *x, *y;
    printf("duplicate keys: %s\n", cJSON_Compare(a, b, 1) ? "equal" : "NOT equal");
    x = cJSON_Parse("[1e999]");
    y = cJSON_Parse("[1e999]");
    printf("infinities: %s\n", cJSON_Compare(x, y, 1) ? "equal" : "NOT equal");
    return 0;
}
