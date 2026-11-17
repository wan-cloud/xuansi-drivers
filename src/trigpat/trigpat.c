/*
 * trigpat.c — the T1 trigger-module reference (LA wave D, ABI 2).
 *
 * A bit-pattern window matcher for the sample (RLE) domain: fire when
 * the window's expanded samples contain `width` CONSECUTIVE samples
 * with (sample & mask) == (value & mask). The run records are expanded
 * arithmetically (a record's value is constant for its whole run, so
 * streak += run and comparing streak >= width is the exact per-tick
 * semantics — the same integer identity the fw's own bits trigger
 * uses). Edge-domain windows (mask/dur_ns records) are not matched:
 * this module returns 0 (never fire) and is documented sample-domain
 * only — it is the agent recipe starting point, not a universal
 * matcher.
 *
 * Determinism contract (README "Trigger modules"): trigger_scan is a
 * PURE function of (window content, config state). No cross-call
 * state, no clocks, no host calls other than la_window_get. The same
 * window bytes + the same config always produce the same decision.
 *
 * Exports (the ABI 2 trigger face — xuansi_host.h "ABI 2 addition"):
 *   xuansi_abi()     -> 2          (the ABI 2 gate)
 *   xuansi_alloc(n)  -> u32        (msgpack marshalling, ABI 1 style)
 *   config(ptr,len)  -> u64 packed ([mask, value, width] -> [ok])
 *                      msgpack in/out per the module convention; called
 *                      via bench.wasm.call BEFORE arming. Defaults if
 *                      never called: mask=0xFF, value=0xA5, width=8.
 *   trigger_scan()   -> i32        (0 = no fire, 1 = fire; the raw
 *                      callback convention — no marshalling on this
 *                      face). Pulls one window via la_window_get and
 *                      matches.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define XUANSI_ABI 2
#include "xuansi_host.h"

#ifndef XUANSI_ABI_OVERRIDE
#define XUANSI_ABI_OVERRIDE XUANSI_ABI
#endif

/* ---- native-test seam (the shared module-test shape) --------------- */
#ifdef XUANSI_NATIVE_TEST
uint8_t *xuansi_lin_base;                  /* test-owned linear-memory base */
#define XUANSI_IN(off) ((const uint8_t *)((size_t)xuansi_lin_base + (size_t)(uint32_t)(off)))
#define XUANSI_OFF(p)  ((uint32_t)((size_t)(p) - (size_t)xuansi_lin_base))
#define XUANSI_EXPORT(name) /* native: plain symbol, no wasm attribute */
static uint8_t *win_buf;                   /* placed by xuansi_native_init() */
static uint8_t *args_slot;
static uint8_t *heap_buf;
static uint8_t *out_buf;
#else
#define XUANSI_IN(off) ((const uint8_t *)(uintptr_t)(off))
#define XUANSI_OFF(p)  ((uint32_t)(uintptr_t)(p))
#define XUANSI_EXPORT(name) __attribute__((export_name(name)))
#endif

/* ---- linear-memory buffers (static footprint = the admission account;
 * build.sh pins --initial-memory=262144 --max-memory=262144) ---- */
#define HEAP_SZ 4096
#define ARGS_SLOT_SZ 128
#define OUT_SZ 256
#define WIN_CAP 4096u       /* fits the fw window (512 records x 5B + 16B
                             * header = 2576B) with headroom; a smaller cap
                             * would still work (truncation keeps the newest
                             * whole records — avail/header accounts it). */
#ifndef XUANSI_NATIVE_TEST
static uint8_t heap_buf[HEAP_SZ] __attribute__((aligned(8)));
static uint8_t args_slot[ARGS_SLOT_SZ] __attribute__((aligned(8)));
static uint8_t out_buf[OUT_SZ] __attribute__((aligned(8)));
static uint8_t win_buf[WIN_CAP] __attribute__((aligned(8)));
#endif
static uint32_t heap_off = 8;      /* 0 must never be returned (bump from 8) */

/* ---- config state (the "init params" of the determinism contract) ---- */
static uint32_t cfg_mask = 0xFF;
static uint32_t cfg_value = 0xA5;
static uint32_t cfg_width = 8;

XUANSI_EXPORT("xuansi_alloc")
uint32_t xuansi_alloc(uint32_t n)
{
    if (n == 0 || n > HEAP_SZ) return 0;
    if (n <= ARGS_SLOT_SZ) return XUANSI_OFF(args_slot);
    uint32_t off = (heap_off + 7u) & ~(uint32_t)7u;
    if (off + n > HEAP_SZ) return 0;
    heap_off = off + n;
    return XUANSI_OFF(heap_buf) + off;
}

XUANSI_EXPORT("xuansi_abi")
uint32_t xuansi_abi(void)
{
    return XUANSI_ABI_OVERRIDE;
}

static uint64_t pack(uint32_t ptr, uint32_t len)
{
    return ((uint64_t)ptr << 32) | (uint64_t)len;
}

/* ---- minimal msgpack decode (the int/array subset — the shared
 * module face) ---- */
static int mp_read_int(const uint8_t *p, uint32_t len, uint32_t *pos,
                       int64_t *out)
{
    if (*pos >= len) return -1;
    uint8_t t = p[(*pos)++];
    if ((t & 0x80) == 0) { *out = t; return 0; }
    if (t >= 0xe0) { *out = (int8_t)t; return 0; }
    uint32_t n = 0;
    if (t == 0xcc) n = 1;
    else if (t == 0xcd) n = 2;
    else if (t == 0xce) n = 4;
    else return -1;
    if (*pos + n > len) return -1;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = (v << 8) | p[(*pos)++];
    *out = (int64_t)v;
    return 0;
}

static int64_t arg_int(const uint8_t *p, uint32_t len, uint32_t idx,
                       int64_t dflt)
{
    uint32_t pos = 0;
    if (pos >= len) return dflt;
    uint8_t t = p[pos++];
    uint32_t n = 0;
    if ((t & 0xf0) == 0x90) n = t & 0x0f;
    else if (t == 0xdc) {
        if (pos + 2 > len) return dflt;
        n = ((uint32_t)p[pos] << 8) | p[pos + 1];
        pos += 2;
    } else {
        return dflt;
    }
    for (uint32_t i = 0; i < n; i++) {
        int64_t v;
        if (mp_read_int(p, len, &pos, &v) != 0) return dflt;
        if (i == idx) return v;
    }
    return dflt;
}

static uint32_t mp_put_int(uint8_t *p, uint32_t pos, uint64_t v)
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

/* config(ptr, len) -> [1]：msgpack [mask, value, width]（width 1..4096；
 * mask/value u8 域檢——域外＝原配置不動、回 [0]）。 */
XUANSI_EXPORT("config")
uint64_t trigpat_config(uint32_t ptr, uint32_t len)
{
    const uint8_t *p = XUANSI_IN(ptr);
    int64_t m = arg_int(p, len, 0, -1);
    int64_t v = arg_int(p, len, 1, -1);
    int64_t w = arg_int(p, len, 2, -1);
    uint32_t okflag = 0;
    if (m >= 0 && m <= 255 && v >= 0 && v <= 255 && w >= 1 && w <= 4096) {
        cfg_mask = (uint32_t)m;
        cfg_value = (uint32_t)v;
        cfg_width = (uint32_t)w;
        okflag = 1;
    }
    out_buf[0] = 0x91;                       /* fixarray(1) */
    uint32_t n = mp_put_int(out_buf, 1, okflag);
    return pack(XUANSI_OFF(out_buf), n);
}

/* ---- window walk ----------------------------------------------------
 * Header (16B, LE): ver u8 / domain u8 / rec_size u16 / avail u32 /
 * seq u64 — see xuansi_host.h. Domain 2 records: {u8 value, u32 run}. */
XUANSI_EXPORT("trigger_scan")
int32_t trigpat_trigger_scan(void)
{
    uint32_t len = la_window_get(XUANSI_OFF(win_buf), WIN_CAP);
    if (len < 16u) return 0;                 /* no window (or header only) */
    if (win_buf[0] != 1u) return 0;          /* ver unknown — never fire */
    if (win_buf[1] != 2u) return 0;          /* sample domain only */
    uint32_t nrec = (len - 16u) / 5u;
    uint32_t mask = cfg_mask, value = cfg_value;
    uint64_t want = cfg_width;
    uint64_t streak = 0;
    for (uint32_t i = 0; i < nrec; i++) {
        const uint8_t *r = &win_buf[16u + i * 5u];
        uint8_t val = r[0];
        uint32_t run;
        memcpy(&run, &r[1], 4);
        if ((uint32_t)(val & mask) == (value & mask)) {
            streak += run;                   /* record value constant over run */
            if (streak >= want) return 1;
        } else {
            streak = 0;
        }
    }
    return 0;
}

#ifdef XUANSI_NATIVE_TEST
void xuansi_native_init(uint8_t *base)
{
    xuansi_lin_base = base;
    win_buf = base + 0x10000;
    args_slot = base + 0x11000;
    heap_buf = base + 0x12000;
    out_buf = base + 0x13000;
    /* config and trigger_scan are driven through the same offsets the
     * wasm host uses; the config statics persist across calls by
     * design (they ARE the module state) — tests order their legs
     * accordingly. */
}
#endif
