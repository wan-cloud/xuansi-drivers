/*
 * test_hclock.c — native unit test for the hclock module (stub host).
 *
 * Build & run (host toolchain, no wasm needed):
 *   cc -DXUANSI_NATIVE_TEST -Wall -Wextra -I src -I src/hclock \
 *      src/hclock/test_hclock.c -o build/test_hclock && ./build/test_hclock
 *
 * Same shape as test_hello.c: one translation unit including the
 * module, a stub `xuansi` namespace, and the u32-offset marshalling
 * the wasm host uses (the test owns the "linear memory" arena via
 * xuansi_native_init()).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "hclock.c"   /* unity build: the module under test */

/* ---- stub host state ---- */

static uint32_t s_millis;                 /* fake clock */

void xuansi_millis(uint32_t *out)
{
    *out = s_millis;
}

/* ---- checks ---- */

static int fails;

static void check(int ok, const char *label)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) fails++;
}

/* decode the module's msgpack result the way the host does */
static uint32_t read_result_u32(uint64_t packed)
{
    uint32_t ptr = (uint32_t)(packed >> 32);
    uint32_t len = (uint32_t)(packed & 0xffffffffu);
    const uint8_t *p = xuansi_lin_base + ptr;
    if (len < 2 || p[0] != 0x91) { fprintf(stderr, "bad result header\n"); exit(2); }
    if (p[1] < 0x80) return p[1];
    if (p[1] == 0xcd) return ((uint32_t)p[2] << 8) | p[3];
    if (p[1] == 0xce)
        return ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16)
             | ((uint32_t)p[4] << 8) | p[5];
    fprintf(stderr, "unexpected int encoding\n");
    exit(2);
}

int main(void)
{
    static uint8_t arena[8192];
    memset(arena, 0xee, sizeof arena);
    xuansi_native_init(arena);

    /* abi gate: the recarved 1 */
    check(xuansi_abi() == 1, "xuansi_abi() == 1 (the recarved ABI 1)");

    /* now() across the three encoding classes */
    s_millis = 42;
    check(read_result_u32(now(0, 0)) == 42, "now() fixint class (< 128)");
    s_millis = 4000;
    check(read_result_u32(now(0, 0)) == 4000, "now() uint16 class");
    s_millis = 4294967295u;   /* the u32 wrap boundary */
    check(read_result_u32(now(0, 0)) == 4294967295u, "now() uint32 class (wrap edge)");

    /* the value must land through the OUT-PARAM convention: a fresh
     * clock read per call (not a stale copy) */
    s_millis = 100; (void)now(0, 0);
    s_millis = 200;
    check(read_result_u32(now(0, 0)) == 200, "now() re-reads millis per call");

    /* alloc convention: small args reuse ONE fixed slot (1000-call
     * high-water stability — the lineage pitfall pinned) */
    uint32_t a1 = xuansi_alloc(40);
    uint32_t a2 = xuansi_alloc(40);
    check(a1 != 0 && a1 == a2, "small allocs reuse the args slot");
    /* large allocs bump and are 8-byte aligned; over-heap refuses with 0 */
    uint32_t b1 = xuansi_alloc(1024);
    uint32_t b2 = xuansi_alloc(1024);
    check(b1 != 0 && b2 != 0 && b1 != b2 && (b1 % 8) == 0, "large allocs bump, aligned");
    check(xuansi_alloc(HEAP_SZ + 1) == 0, "over-heap alloc refuses (0)");
    check(xuansi_alloc(0) == 0, "zero alloc refuses (0)");
    for (int i = 0; i < 1000; i++) { (void)xuansi_alloc(40); }
    check(xuansi_alloc(40) == a1, "1000 small calls: slot stable (no bump leak)");

    printf("%s: %s\n", fails ? "FAIL" : "PASS", fails ? "hclock native test" : "hclock native test");
    return fails ? 1 : 0;
}
