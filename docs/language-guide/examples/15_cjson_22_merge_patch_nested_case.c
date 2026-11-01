//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: merge patch: none
// generate_merge_patch calls itself through the public case-insensitive wrapper, so the case-sensitive entry point is
// case-sensitive only at the top level. Two levels down, "a" and "A" count as the same key and the difference is lost.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *from = cJSON_Parse("{\"x\":{\"y\":{\"a\":1}}}");
    cJSON *to = cJSON_Parse("{\"x\":{\"y\":{\"A\":1}}}");
    cJSON *patch = cJSONUtils_GenerateMergePatchCaseSensitive(from, to);
    if (patch == NULL) {
        printf("merge patch: none\n");
    } else {
        printf("merge patch %s\n", cJSON_PrintUnformatted(patch));
    }
    return 0;
}
