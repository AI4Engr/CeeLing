//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: found 10
// An empty token after the last "/" is accepted as the index 0: the pointer "/a/" names the first element of "a".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":[10,20]}");
    cJSON *found = cJSONUtils_GetPointer(doc, "/a/");
    if (found == NULL) {
        printf("nothing\n");
    } else {
        printf("found %s\n", cJSON_PrintUnformatted(found));
    }
    return 0;
}
