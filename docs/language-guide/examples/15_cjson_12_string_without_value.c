//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: path->valuestring[0] with valuestring == NULL
// apply_patch checks that "path" is a string (cJSON_IsString looks at the type only) and then reads
// path->valuestring[0]. A string node built with cJSON_CreateStringReference(NULL) has type String and no text.
#include <stdio.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1}");
    cJSON *patch = cJSON_CreateObject();
    cJSON *patches = cJSON_CreateArray();
    cJSON_AddStringToObject(patch, "op", "remove");
    cJSON_AddItemToObject(patch, "path", cJSON_CreateStringReference(NULL));
    cJSON_AddItemToArray(patches, patch);
    printf("applying\n");
    fflush(stdout);
    printf("status %d\n", cJSONUtils_ApplyPatches(doc, patches));
    return 0;
}
