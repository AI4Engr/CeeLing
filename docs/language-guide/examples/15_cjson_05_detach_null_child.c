//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ crash: parent->child->prev = item->prev with parent->child == NULL
// DetachItemViaPointer: the claimed parent has no child, but the item has a prev. The check
// (item != parent->child && item->prev == NULL) passes, and the last branch writes through parent->child.
#include <stdio.h>
#include "cJSON.h"
int main(void) {
    cJSON *parent = cJSON_CreateArray();
    cJSON *other = cJSON_CreateNumber(1.0);
    cJSON *item = cJSON_CreateNumber(2.0);
    item->prev = other;
    printf("detaching\n");
    fflush(stdout);
    cJSON_DetachItemViaPointer(parent, item);
    printf("survived\n");
    return 0;
}
