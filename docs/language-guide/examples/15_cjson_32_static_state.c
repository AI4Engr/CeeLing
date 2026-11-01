//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: caller A: parse failed, error at 3
//@ run: caller B: parse succeeded
//@ run: caller A asks again: the error pointer is NULL: 1
// cJSON keeps the position of the last parse error in one file-scope variable (global_error), and cJSON_Version
// formats into one static buffer. Every caller shares them. Here two "callers" run one after the other in one
// thread: caller B's successful parse resets the record that caller A is about to read. With two threads the
// same sharing is a data race, which this program does not demonstrate.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    const char *textA = "[1,";
    const char *textB = "[2]";
    cJSON *a = cJSON_Parse(textA);
    printf("caller A: parse failed, error at %d\n", (int)(cJSON_GetErrorPtr() - textA));
    a = cJSON_Parse(textB);                       /* caller B, somewhere else in the program */
    printf("caller B: parse %s\n", a != NULL ? "succeeded" : "failed");
    printf("caller A asks again: the error pointer is NULL: %d\n", cJSON_GetErrorPtr() == NULL);
    return 0;
}
