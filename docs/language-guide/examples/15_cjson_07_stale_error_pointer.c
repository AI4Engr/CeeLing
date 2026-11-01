//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: failed at 3
//@ run: after cJSON_ParseWithOpts(NULL): item=0, the error pointer is still the old one: 1
// cJSON_ParseWithOpts(NULL, ...) returns before the code that resets the saved error, so cJSON_GetErrorPtr()
// still points into the buffer of the earlier call, which the caller has since freed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
int main(void) {
    char *buffer = (char *)malloc(8);
    const char *old_error;
    cJSON *item;
    strcpy(buffer, "[1,");
    item = cJSON_Parse(buffer);
    old_error = cJSON_GetErrorPtr();
    printf("failed at %d\n", (int)(old_error - buffer));
    free(buffer);                                   /* the caller is done with its text */
    item = cJSON_ParseWithOpts(NULL, NULL, 0);      /* "nothing to parse": returns early */
    printf("after cJSON_ParseWithOpts(NULL): item=%d, the error pointer is still the old one: %d\n",
           item != NULL, cJSON_GetErrorPtr() == old_error);
    return 0;
}
