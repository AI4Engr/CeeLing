//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: minified {"a":"x\\","b": 1 }
// Minify copies a string literal character by character and treats a backslash followed by a quote as an escaped quote,
// without asking whether that backslash is itself escaped. The string "x\\" (an escaped backslash, then the real closing
// quote) therefore swallows its closing quote, and the rest of the text is taken for string content.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    char buffer[] = "{\"a\":\"x\\\\\",\"b\": 1 }";
    cJSON_Minify(buffer);
    printf("minified %s\n", buffer);
    return 0;
}
