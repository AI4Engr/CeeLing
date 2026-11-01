//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: apply_patch reads object->string with object == NULL
// A root-level "add" applied to a NULL object. overwrite_item returns early for NULL, then apply_patch
// reads object->string without a test.
#include <stdio.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *patches = cJSON_Parse("[{\"op\":\"add\",\"path\":\"\",\"value\":1}]");
    printf("applying\n");
    fflush(stdout);
    printf("survived status %d\n", cJSONUtils_ApplyPatches(NULL, patches));
    return 0;
}
