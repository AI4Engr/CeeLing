//@ c: compiles
//@ project: cjson-cling
//@ sources: cJSON.c cJSON_Utils.c
//@ run: printed ["ab"]
// parse_hex4 returns 0 for an invalid digit, which is also the value of a real "\u0000". The escape \u12G4 is accepted
// (as the character U+0000) and the string is cut there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "cJSON_Utils.h"
int main(void) {
    cJSON *item = cJSON_Parse("[\"ab\\u12G4cd\"]");
    printf("printed %s\n", cJSON_PrintUnformatted(item));
    return 0;
}
