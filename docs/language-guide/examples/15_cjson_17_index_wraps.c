//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: found 10
// decode_array_index_from_pointer adds up the digits in a size_t with no overflow check, so 2^64 wraps around to 0
// and the pointer "/18446744073709551616" names the first element.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *doc = cJSON_Parse("[10,20,30]");
    cJSON *found = cJSONUtils_GetPointer(doc, "/18446744073709551616");
    if (found == NULL) {
        printf("nothing\n");
    } else {
        printf("found %s\n", cJSON_PrintUnformatted(found));
    }
    return 0;
}
