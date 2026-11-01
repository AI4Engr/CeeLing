//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: status 7 doc {}
// apply_patch detaches and deletes the target of a "replace" before it looks for the "value": a replace without one
// fails with status 7 after it has already destroyed the member.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"replace\",\"path\":\"/a\"}]");
    int status = cJSONUtils_ApplyPatches(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
