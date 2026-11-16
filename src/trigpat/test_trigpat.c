/*
 * test_trigpat.c — native unit test for the trigpat trigger module
 * (stub host; same shape as test_hello.c).
 *
 * Build & run (host toolchain, no wasm needed):
 *   cc -DXUANSI_NATIVE_TEST -Wall -Wextra -Werror -I src -I src/trigpat \
 *      src/trigpat/test_trigpat.c -o build/test_trigpat && ./build/test_trigpat
 *
 * The test includes trigpat.c directly, stubs the `xuansi` namespace
 * (only la_window_get is consumed), and drives config() through the
 * same msgpack-at-offset marshalling the wasm host uses. Window
 * scenarios are built in the fw window format (16B header + 5B
 * {u8 value, u32 run} records, oldest first) and served by the stub.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>

#include "trigpat.c"   /* unity build: the module under test */

/* ---- stub host state ---- */

static uint8_t  s_arena[0x20000];          /* test-owned linear memory */
static uint8_t  s_win[2600];               /* scenario window (fw format) */
static uint32_t s_win_len;
static int      s_win_served;              /* la_window_get call counter */
static int      s_win_refuse;              /* emulate "no scan window" */

static jmp_buf s_jmp;
static int     s_trap_armed;
static char    s_trap_msg[128];
static void stub_raise(const char *msg)
{
    if (!s_trap_armed) {
        fprintf(stderr, "FATAL: stub host raised outside an armed trap:"
                        " %s\n", msg);
        exit(2);
    }
    snprintf(s_trap_msg, sizeof s_trap_msg, "%s", msg);
    longjmp(s_jmp, 1);
}

/* The ONE host function trigpat consumes. Mirrors the fw contract:
 * bounds-checked buf, direct u32 return, copy at most cap bytes of the
 * current round's window. */
uint32_t xuansi_la_window_get(uint32_t buf_off, uint32_t cap)
{
    s_win_served++;
    if (s_win_refuse) {
        stub_raise("xuansi.la_window_get: no scan window (only callable"
                   " inside trigger_scan during an armed module-trigger"
                   " capture)");
    }
    if (buf_off > sizeof s_arena || cap > sizeof s_arena - buf_off) {
        stub_raise("xuansi.la_window_get: buf out of bounds");
    }
    uint32_t n = s_win_len < cap ? s_win_len : cap;
    memcpy(xuansi_lin_base + buf_off, s_win, n);
    return n;
}

/* ---- msgpack helper for driving config() exactly like the host ---- */

static uint8_t s_args[128];

static uint32_t put_int(uint8_t *p, uint32_t pos, uint64_t v)
{
    if (v < 128) { p[pos++] = (uint8_t)v; return pos; }
    if (v < 0x10000) {
        p[pos++] = 0xcd; p[pos++] = (uint8_t)(v >> 8); p[pos++] = (uint8_t)v;
        return pos;
    }
    p[pos++] = 0xce;
    p[pos++] = (uint8_t)(v >> 24); p[pos++] = (uint8_t)(v >> 16);
    p[pos++] = (uint8_t)(v >> 8); p[pos++] = (uint8_t)v;
    return pos;
}

static uint32_t args3(int64_t a, int64_t b, int64_t c)
{
    s_args[0] = 0x93;                       /* fixarray(3) */
    uint32_t n = 1;
    n = put_int(s_args, n, (uint64_t)a);
    n = put_int(s_args, n, (uint64_t)b);
    n = put_int(s_args, n, (uint64_t)c);
    return n;
}

/* build a domain-2 window from (value, run) pairs */
static void win_build(const uint8_t *vals, const uint32_t *runs, uint32_t n,
                      uint32_t avail_extra)
{
    s_win[0] = 1;                           /* ver */
    s_win[1] = 2;                           /* domain = sample */
    s_win[2] = 5; s_win[3] = 0;             /* rec_size */
    uint32_t avail = n + avail_extra;
    memcpy(&s_win[4], &avail, 4);
    uint64_t seq = 0;
    memcpy(&s_win[8], &seq, 8);
    s_win_len = 16;
    for (uint32_t i = 0; i < n; i++) {
        s_win[s_win_len] = vals[i];
        memcpy(&s_win[s_win_len + 1], &runs[i], 4);
        s_win_len += 5;
    }
}

static int s_fails;
#define CHECK(cond, name) do { \
    if (cond) { printf("ok   %s\n", name); } \
    else { printf("FAIL %s\n", name); s_fails++; } \
} while (0)

/* config via the marshalling convention; returns the ok flag (or -1) */
static int drive_config(int64_t m, int64_t v, int64_t w)
{
    uint32_t n = args3(m, v, w);
    uint32_t ptr = xuansi_alloc(n);
    if (ptr == 0) return -1;
    memcpy(xuansi_lin_base + ptr, s_args, n);
    uint64_t packed = trigpat_config(ptr, n);
    uint32_t rptr = (uint32_t)(packed >> 32);
    uint32_t rlen = (uint32_t)(packed & 0xffffffffu);
    const uint8_t *r = xuansi_lin_base + rptr;
    if (rlen < 2 || r[0] != 0x91) return -1;   /* fixarray(1) + int */
    return (r[1] & 0x80) == 0 ? r[1] : -1;     /* pos fixint payload */
}

int main(void)
{
    xuansi_native_init(s_arena);

    /* ---- defaults (mask=0xFF value=0xA5 width=8) ---- */
    {
        uint8_t vals[3] = { 0x00, 0xA5, 0xFF };
        uint32_t runs[3] = { 10, 8, 4 };
        win_build(vals, runs, 3, 0);
        CHECK(trigpat_trigger_scan() == 1,
              "defaults: A5-run-8 fires (streak==width exactly)");
    }
    {
        uint8_t vals[2] = { 0xA5, 0x00 };
        uint32_t runs[2] = { 7, 20 };
        win_build(vals, runs, 2, 0);
        CHECK(trigpat_trigger_scan() == 0,
              "defaults: A5-run-7 no fire (one sample short)");
    }

    /* ---- config() through the marshalling convention ---- */
    CHECK(drive_config(0x0F, 0x05, 4) == 1, "config accepts valid triple");
    {
        uint8_t vals[3] = { 0x35, 0x05, 0x35 };
        uint32_t runs[3] = { 1, 3, 1 };
        win_build(vals, runs, 3, 0);
        /* mask 0x0F value 0x05: 0x35&0x0F=5 and 0x05 both match ->
         * streak 1+3=4 hits width 4 ACROSS records (RLE arithmetic) */
        CHECK(trigpat_trigger_scan() == 1,
              "masked cross-record streak fires (1+3>=4)");
    }
    {
        uint8_t vals[3] = { 0x35, 0x46, 0x35 };
        uint32_t runs[3] = { 2, 9, 2 };
        win_build(vals, runs, 3, 0);
        /* 0x46&0x0F=6 != 5 resets; totals 2 and 2 never reach 4 */
        CHECK(trigpat_trigger_scan() == 0,
              "streak reset by non-match record");
    }
    CHECK(drive_config(300, 0, 1) == 0, "config rejects mask>255");
    {
        uint8_t vals[1] = { 0xA5 };
        uint32_t runs[1] = { 8 };
        win_build(vals, runs, 1, 0);
        /* rejected config kept the previous one (0x0F/0x05/4):
         * 0xA5&0x0F=5, run 8 >= 4 -> still fires */
        CHECK(trigpat_trigger_scan() == 1,
              "rejected config keeps previous state");
    }

    /* ---- width beyond the window's matched total: no fire ---- */
    CHECK(drive_config(0xFF, 0x00, 4096) == 1, "config width=4096 accepted");
    {
        uint8_t vals[2] = { 0x00, 0x00 };
        uint32_t runs[2] = { 100, 100 };
        win_build(vals, runs, 2, 0);
        CHECK(trigpat_trigger_scan() == 0,
              "matched total 200 < width 4096: no fire");
    }

    /* ---- domain / header guards ---- */
    {
        uint8_t vals[1] = { 0xA5 };
        uint32_t runs[1] = { 64 };
        win_build(vals, runs, 1, 0);
        s_win[1] = 1;                       /* domain = edge */
        CHECK(trigpat_trigger_scan() == 0, "edge-domain window never fires");
        s_win[1] = 2;
        s_win[0] = 2;                       /* unknown ver */
        CHECK(trigpat_trigger_scan() == 0, "unknown window ver never fires");
        s_win[0] = 1;
        s_win_len = 8;                      /* short window */
        CHECK(trigpat_trigger_scan() == 0, "short window (<16B) no fire");
    }

    /* ---- truncation by module cap: decides on delivered records ---- */
    {
        uint8_t vals[3] = { 0xFF, 0x00, 0xA5 };
        uint32_t runs[3] = { 5, 5, 6 };
        win_build(vals, runs, 3, 0);
        CHECK(drive_config(0xFF, 0xA5, 6) == 1, "config for truncation leg");
        /* emulate a tiny fw cap: header + only the NEWEST record ride
         * (the fw keeps the newest whole records on truncation) */
        memmove(s_win + 16, s_win + 16 + 2 * 5, 5);
        s_win_len = 16 + 5;
        /* header still claims avail=3 — the module matches on the one
         * delivered A5 run 6 record */
        CHECK(trigpat_trigger_scan() == 1,
              "truncated window decides on delivered (newest) records");
    }

    /* ---- xuansi_abi / alloc conventions ---- */
    CHECK(xuansi_abi() == 2, "abi declares 2");
    CHECK(xuansi_alloc(0) == 0 && xuansi_alloc(HEAP_SZ + 1) == 0,
          "alloc domain: 0 and >heap refused");
    CHECK(xuansi_alloc(64) != 0, "alloc hands out a real offset");

    /* ---- la_window_get refusal path (call outside a scan) ---- */
    {
        s_win_refuse = 1;
        s_trap_armed = 1;
        if (setjmp(s_jmp) == 0) {
            (void)trigpat_trigger_scan();
            s_trap_armed = 0;
            CHECK(0, "refused window must trap");
        } else {
            s_trap_armed = 0;
            CHECK(strstr(s_trap_msg, "no scan window") != NULL,
                  "refused window traps with the host text");
        }
        s_win_refuse = 0;
    }

    CHECK(s_win_served > 0, "la_window_get was actually consumed");

    printf(s_fails ? "%d FAIL\n" : "all ok (%d)\n",
           s_fails ? s_fails : s_win_served);
    return s_fails ? 1 : 0;
}
