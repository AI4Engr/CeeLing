//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: printed [0.3,123456789.12345679,1]
// print_number prints 15 significant digits and keeps them if reading them back gives a value within DBL_EPSILON
// (relative) of the original. That test accepts output that is not the number: 0.30000000000000004 prints as 0.3.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *item = cJSON_Parse("[0.30000000000000004,123456789.123456789,1.0000000000000002]");
    printf("printed %s\n", cJSON_PrintUnformatted(item));
    return 0;
}
