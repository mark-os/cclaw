#include <stdio.h>
#include "cclaw.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("cclaw %s\n", CCLAW_VERSION);
    return 0;
}
