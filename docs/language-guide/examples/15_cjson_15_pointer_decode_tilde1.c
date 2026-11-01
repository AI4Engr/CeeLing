//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: status 0 doc {"a~/":1}
// decode_pointer_inplace writes the "/" for "~1" through the wrong index and never copies ordinary characters after
// an escape, so the key that the pointer "/a~1b" names is not "a/b".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{}");
    cJSON *patches = cJSON_Parse("[{\"op\":\"add\",\"path\":\"/a~1b\",\"value\":1}]");
    int status = cJSONUtils_ApplyPatches(doc, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
