//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: status 0 doc {"a":1,"":9}
// overwrite_item copies the whole replacement struct over the node (memcpy), so a root-level replace applied to
// a member (here "b") also overwrites the member's next and prev links and its key. "c" is cut off, "b" has
// lost its name; the nodes dropped from the list are leaked.
#include <stdio.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1,\"b\":2,\"c\":3}");
    cJSON *b = cJSON_GetObjectItem(doc, "b");
    cJSON *patches = cJSON_Parse("[{\"op\":\"replace\",\"path\":\"\",\"value\":9}]");
    int status = cJSONUtils_ApplyPatches(b, patches);
    printf("status %d doc %s\n", status, cJSON_PrintUnformatted(doc));
    return 0;
}
