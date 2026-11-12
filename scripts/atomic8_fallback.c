/* 8-byte atomic helpers for static builds on CPUs without a 64-bit CAS
 * (ARMv5TE). Linked only by scripts/build_static.sh, only when its link probe
 * says the compiler leaves these as library calls.
 *
 * Normally libatomic supplies them, and the dynamic armv5te build still uses
 * it. A static musl binary cannot: the distro's ARM libatomic selects its
 * implementations through IFUNC, which musl does not resolve, so the first
 * call would jump through an unrelocated pointer.
 *
 * The only caller is QuickJS's Atomics on 64-bit typed arrays. One global
 * lock is what libatomic's own fallback does; the 1-byte test-and-set under
 * it comes from libgcc's kernel-helper routines, which need no IFUNC. */
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>

static volatile char g_lock;

static void lock(void) {
    while (__sync_lock_test_and_set(&g_lock, 1)) sched_yield();
}
static void unlock(void) {
    __sync_lock_release(&g_lock);
}

uint64_t __atomic_load_8(const volatile void *p, int order) {
    (void)order;
    lock();
    uint64_t v = *(const volatile uint64_t *)p;
    unlock();
    return v;
}

void __atomic_store_8(volatile void *p, uint64_t val, int order) {
    (void)order;
    lock();
    *(volatile uint64_t *)p = val;
    unlock();
}

/* The library entry point takes no `weak` argument, unlike the builtin of the
 * same name, so it is defined under another C name and bound with an asm
 * label (libatomic does the same). */
bool cas8(volatile void *p, void *expected, uint64_t desired, int success, int failure)
    __asm__("__atomic_compare_exchange_8");
bool cas8(volatile void *p, void *expected, uint64_t desired, int success, int failure) {
    (void)success; (void)failure;
    volatile uint64_t *ptr = p;
    uint64_t *exp = expected;
    lock();
    bool same = *ptr == *exp;
    if (same) *ptr = desired; else *exp = *ptr;
    unlock();
    return same;
}

/* exchange and the fetch_<op> family: return the old value, store the new. */
#define FETCH_OP(name, expr)                                          \
    uint64_t name(volatile void *p, uint64_t val, int order) {        \
        (void)order;                                                  \
        volatile uint64_t *ptr = p;                                   \
        lock();                                                       \
        uint64_t old = *ptr;                                          \
        *ptr = (expr);                                                \
        unlock();                                                     \
        return old;                                                   \
    }

FETCH_OP(__atomic_exchange_8,  val)
FETCH_OP(__atomic_fetch_add_8, old + val)
FETCH_OP(__atomic_fetch_sub_8, old - val)
FETCH_OP(__atomic_fetch_and_8, old & val)
FETCH_OP(__atomic_fetch_or_8,  old | val)
FETCH_OP(__atomic_fetch_xor_8, old ^ val)
