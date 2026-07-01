#define _POSIX_C_SOURCE 200809L
#include "http_policy.h"
#include <arpa/inet.h>
#include <string.h>

int http_is_private_ip(const char *ip) {
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip, &addr4) == 1) {
        uint32_t h = ntohl(addr4.s_addr);
        if ((h >> 24) == 127) return 1;
        if ((h >> 24) == 10) return 1;
        if ((h >> 24) == 0) return 1;                    /* 0.0.0.0/8 */
        if ((h & 0xFFF00000) == 0xAC100000) return 1;   /* 172.16.0.0/12 */
        if ((h >> 16) == ((192 << 8) | 168)) return 1;
        if ((h >> 16) == ((169 << 8) | 254)) return 1;
        if ((h & 0xFFC00000) == 0x64400000) return 1;   /* 100.64.0.0/10 */
        return 0;
    }
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip, &addr6) == 1) {
        static const uint8_t lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (memcmp(&addr6, lo, 16) == 0) return 1;
        static const uint8_t zero[16] = {0};
        if (memcmp(&addr6, zero, 16) == 0) return 1;
        if (addr6.s6_addr[0] == 0xfe && (addr6.s6_addr[1] & 0xc0) == 0x80) return 1;
        if ((addr6.s6_addr[0] & 0xfe) == 0xfc) return 1;
        /* IPv4-mapped IPv6: ::ffff:x.x.x.x */
        static const uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (memcmp(addr6.s6_addr, mapped, 12) == 0) {
            char v4[INET_ADDRSTRLEN];
            struct in_addr embedded;
            memcpy(&embedded, &addr6.s6_addr[12], 4);
            inet_ntop(AF_INET, &embedded, v4, sizeof(v4));
            return http_is_private_ip(v4);
        }
        return 0;
    }
    return 0;
}
