//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: {a:1}: error at 2
//@ run: [1,x]: error at 3
// parse_string moves past the opening quote before it checks that there is one, so the position it reports for a
// malformed key or value is one byte too far.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
static void show(const char *text) {
    const char *end = NULL;
    cJSON *item = cJSON_ParseWithOpts(text, &end, 0);
    if (item != NULL) {
        printf("%s: parsed\n", text);
    } else {
        printf("%s: error at %d\n", text, (int)(end - text));
    }
}

int main(void) {
    show("{a:1}");
    show("[1,x]");
    return 0;
}
