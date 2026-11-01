//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: printed [null,0,null]
// parse_number does not look at the range error strtod reports: 1e999 becomes infinity (printed as null) and 1e-999
// becomes 0.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *item = cJSON_Parse("[1e999,1e-999,-1e999]");
    printf("printed %s\n", cJSON_PrintUnformatted(item));
    return 0;
}
