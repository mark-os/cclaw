#include <assert.h>
#include <stdio.h>
#include "http_policy.h"
#include "test_util.h"

/* is_private_ip: what the egress check treats as an internal address. */
static void test_is_private_ip(void) {
    static const struct { const char *ip; int priv; } cases[] = {
        { "127.0.0.1", 1 }, { "10.0.0.1", 1 }, { "172.16.0.1", 1 }, { "172.31.255.255", 1 },
        { "192.168.1.1", 1 }, { "169.254.1.1", 1 },
        { "100.64.0.1", 1 }, { "100.127.255.255", 1 },              /* CGNAT */
        { "0.0.0.0", 1 }, { "0.1.2.3", 1 },
        { "::1", 1 }, { "fe80::1", 1 }, { "fc00::1", 1 }, { "fd12::34", 1 },
        { "::ffff:127.0.0.1", 1 }, { "::ffff:10.0.0.1", 1 },
        { "8.8.8.8", 0 }, { "1.1.1.1", 0 }, { "100.63.255.255", 0 }, { "100.128.0.0", 0 },
        { "172.15.0.1", 0 }, { "172.32.0.1", 0 }, { "2606:4700:4700::1111", 0 },
        { "::ffff:8.8.8.8", 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = http_is_private_ip(cases[i].ip) ? 1 : 0;
        if (got != cases[i].priv)
            fprintf(stderr, "%s: private=%d, want %d\n", cases[i].ip, got, cases[i].priv);
        assert(got == cases[i].priv);
    }
}

int main(void) {
    TEST_INIT();
    RUN(test_is_private_ip);
    printf("test_http_policy: all tests passed\n");
    return 0;
}
