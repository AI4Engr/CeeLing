//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: merge patch {"B":1,"a":null,"B":null}
// generate_merge_patch sorts both objects with the comparator it was given (case-insensitive for GenerateMergePatch) and
// then walks them with plain strcmp. Where the two orders disagree ("a" sorts before "B", but strcmp puts "B" first) the
// walk pairs the wrong keys, and the patch both keeps and removes "B".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *from = cJSON_Parse("{\"a\":1,\"B\":1}");
    cJSON *to = cJSON_Parse("{\"B\":1}");
    cJSON *patch = cJSONUtils_GenerateMergePatch(from, to);
    if (patch == NULL) {
        printf("merge patch: none\n");
    } else {
        printf("merge patch %s\n", cJSON_PrintUnformatted(patch));
    }
    return 0;
}
