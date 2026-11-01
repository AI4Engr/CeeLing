//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: the tree now contains itself; printing it recurses until the stack is gone
// ReplaceItemViaPointer with the item's own parent as the replacement: the parent becomes its own child,
// and the function reports success.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *root = cJSON_Parse("{\"k\":{\"x\":1}}");
    cJSON *k = cJSON_GetObjectItem(root, "k");
    cJSON_bool ok = cJSON_ReplaceItemViaPointer(root, k, root);
    printf("replace returned %d, root->child == root: %d\n", (int)ok, root->child == root);
    fflush(stdout);
    printf("printed %s\n", cJSON_PrintUnformatted(root));
    return 0;
}
