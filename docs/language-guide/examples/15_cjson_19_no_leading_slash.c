//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: the root
// get_item_from_pointer follows a pointer only while it starts with "/": any other non-empty text, such as "a", walks
// zero steps and returns the root.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("{\"a\":1}");
    cJSON *found = cJSONUtils_GetPointer(doc, "a");
    if (found == doc) {
        printf("the root\n");
    } else if (found == NULL) {
        printf("nothing\n");
    } else {
        printf("found %s\n", cJSON_PrintUnformatted(found));
    }
    return 0;
}
