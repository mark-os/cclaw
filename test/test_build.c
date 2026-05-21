#include <stdio.h>
#include <stdlib.h>
#include "cclaw.h"

int main(void) {
    /* Verify version string exists */
    if (CCLAW_VERSION[0] == '\0') {
        fprintf(stderr, "FAIL: CCLAW_VERSION empty\n");
        return 1;
    }
    printf("PASS: build ok, version=%s\n", CCLAW_VERSION);
    return 0;
}
