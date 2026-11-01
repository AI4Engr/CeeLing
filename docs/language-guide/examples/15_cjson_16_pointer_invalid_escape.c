//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: status 0 doc {"a~2b":1}
// An invalid "~" escape (here "~2") stops decode_pointer_inplace quietly: it neither fails the patch nor decodes the
// rest of the key.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"add\",\"path\":\"/a~2b\",\"value\":1}]");
    int status = cJSONUtils_ApplyPatches(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
