//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: nan->-2147483648 1e30->2147483647
// cJSON_CreateNumber saturates a double that is >= INT_MAX or <= INT_MIN, then falls through to (int)num. For a
// NaN both comparisons are false, so (int)NaN is evaluated, which is undefined behaviour in C. The value printed
// here, INT_MIN, is what x86-64 produces for it; the standard promises nothing, and another CPU may print 0.
#include <math.h>
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    volatile double zero = 0.0;
    cJSON *n = cJSON_CreateNumber(zero / zero);
    cJSON *big = cJSON_CreateNumber(1.0e30);
    printf("nan->%d 1e30->%d\n", n->valueint, big->valueint);
    return 0;
}
