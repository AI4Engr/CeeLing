//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: status 0 doc {"b":2}
// detach_path finds the parent with the case flag it was given, but removes the child with
// cJSON_DetachItemFromObject, which ignores case: in the case-sensitive entry point "remove /a" removes the member "A".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"A\":1,\"b\":2}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"remove\",\"path\":\"/a\"}]");
    int status = cJSONUtils_ApplyPatchesCaseSensitive(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
