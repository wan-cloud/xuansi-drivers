/*
 * hclock.c — the millis one-native module (the bench clock as a wasm
 * module; kind=driver, release=production).
 *
 * Canonical home: wan-cloud/xuansi-drivers (src/hclock/hclock.c).
 *
 * Lineage (recorded honestly): hclock was first created ON BOARD by an
 * agent (B14, 2026-11-11 — the "write hclock" session that had to copy
 * hello.c out of the then-private firmware tree and hit two template
 * pitfalls: a missing <stdint.h> include, and a 256B heap exhausted by
 * the args bump ~230 calls in). v0.0.1/0.0.2 lived only in the daemon
 * workspace and board #1 /drv; this file is the repo-native
 * reconstruction for the recarved ABI 1 (B15 W2-T3, 2026-11-12) —
 * same face, the safe alloc convention (args-slot reuse + 8KB bump
 * arena) from the start. The staged product lands in staging/ until
 * the W3-T2 republish.
 *
 * Why it existed: pre-B15 the Lua face had NO timing source, so timing
 * loops had to round-trip through a wasm module calling the millis
 * native. Since B15 W1-T3 the board exposes bench.millis() directly —
 * this module remains as the smallest reference for a one-native
 * module (and the cheapest load-path smoke: ~700B interp).
 *
 * Calling face (recarved ABI 1 marshalling; one msgpack array in, one
 * msgpack array out):
 *
 *   bench.wasm.call("hclock", "now")  -> {ms}   boot milliseconds (u32)
 *
 * The single native used: xuansi.millis (out-param convention — the
 * value lands in a .bss scalar, never a wasm stack slot).
 */
#include <stdint.h>
#include <stddef.h>

#include "xuansi_host.h"

#ifndef XUANSI_ABI_OVERRIDE
#define XUANSI_ABI_OVERRIDE XUANSI_ABI   /* 1 — the recarved ABI 1 (B15) */
#endif

/* ---- native-test seam (same discipline as hello.c) ---------------- */

#ifdef XUANSI_NATIVE_TEST
uint8_t *xuansi_lin_base;
#define XUANSI_IN(off) ((const uint8_t *)((size_t)xuansi_lin_base + (size_t)(uint32_t)(off)))
#define XUANSI_OFF(p)  ((uint32_t)((size_t)(p) - (size_t)xuansi_lin_base))
#define XUANSI_EXPORT(name) /* native: plain symbol, no wasm attribute */
#define LIN_OUT_OFF   0x100u              /* result marshal buffer */
#define LIN_ARGS_OFF  0x200u              /* reused args slot */
#define LIN_HEAP_OFF  0x300u              /* bump arena */
void xuansi_native_init(uint8_t *base);
#else
#define XUANSI_IN(off) ((const uint8_t *)(uintptr_t)(off))
#define XUANSI_OFF(p)  ((uint32_t)(uintptr_t)(p))
#define XUANSI_EXPORT(name) __attribute__((export_name(name)))
#endif

/* ---- in-linear-memory marshal buffers (hello.c's safe convention) --
 * The host calls xuansi_alloc on EVERY call-with-args: small args
 * (<= ARGS_SLOT_SZ) reuse one fixed slot, larger ones use the bump
 * arena — never returned, by contract (a bump-only allocator is a
 * hidden per-call budget; the lineage note above records the 256B-heap
 * variant that died of exactly this). */

#define HEAP_SZ 8192
#define ARGS_SLOT_SZ 128
#ifdef XUANSI_NATIVE_TEST
static uint32_t heap_off = 8;      /* 0 must never be returned (bump from 8) */
static uint8_t *args_slot;         /* placed by xuansi_native_init() */
static uint8_t *heap_buf;          /* placed by xuansi_native_init() */
static uint8_t *out_buf;
#else
static uint8_t heap_buf[HEAP_SZ] __attribute__((aligned(8)));
static uint8_t args_slot[ARGS_SLOT_SZ] __attribute__((aligned(8)));
static uint32_t heap_off = 8;      /* 0 must never be returned (bump from 8) */
static uint8_t out_buf[64] __attribute__((aligned(8)));
#endif

static uint32_t g_ms;              /* out-param landing spot: linear .bss
                                    * (wasm stack slots live on the execution
                                    * page, out of the host's write reach on
                                    * the AOT lane — cross-lane portability) */

XUANSI_EXPORT("xuansi_alloc")
uint32_t xuansi_alloc(uint32_t n)
{
    if (n == 0 || n > HEAP_SZ) return 0;
    if (n <= ARGS_SLOT_SZ) return XUANSI_OFF(args_slot);   /* reuse */
    uint32_t off = (heap_off + 7u) & ~(uint32_t)7u;
    if (off + n > HEAP_SZ) return 0;
    heap_off = off + n;
    return XUANSI_OFF(heap_buf) + off;   /* heap_buf-based (P3-T2 rider) */
}

XUANSI_EXPORT("xuansi_abi")
uint32_t xuansi_abi(void)
{
    return XUANSI_ABI_OVERRIDE;   /* 1 — the recarved ABI 1 (B15, 2026-11-11) */
}

static uint64_t pack(uint32_t ptr, uint32_t len)
{
    return ((uint64_t)ptr << 32) | (uint64_t)len;
}

/* now() -> [ms]: one millis out-param read, one-element msgpack array
 * back. Encoding: fixarray(1) + fixint/uint16/uint32 (the minimal int
 * face — ms wraps u32 at 49.7 days, matching the native). */
XUANSI_EXPORT("now")
uint64_t now(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    millis(&g_ms);
    uint32_t v = g_ms;
    uint32_t pos = 0;
    out_buf[pos++] = 0x91;                      /* fixarray(1) */
    if (v < 128) {
        out_buf[pos++] = (uint8_t)v;            /* pos fixint */
    } else if (v < 0x10000) {
        out_buf[pos++] = 0xcd;                  /* uint16 */
        out_buf[pos++] = (uint8_t)(v >> 8);
        out_buf[pos++] = (uint8_t)v;
    } else {
        out_buf[pos++] = 0xce;                  /* uint32 */
        out_buf[pos++] = (uint8_t)(v >> 24);
        out_buf[pos++] = (uint8_t)(v >> 16);
        out_buf[pos++] = (uint8_t)(v >> 8);
        out_buf[pos++] = (uint8_t)v;
    }
    return pack(XUANSI_OFF(out_buf), pos);
}

#ifdef XUANSI_NATIVE_TEST
void xuansi_native_init(uint8_t *base)
{
    xuansi_lin_base = base;
    out_buf = base + LIN_OUT_OFF;
    args_slot = base + LIN_ARGS_OFF;
    heap_buf = base + LIN_HEAP_OFF;
}
#endif
