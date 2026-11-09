/*
 * hello.c — reference module for module_abi=1 (the ABI's first consumer).
 *
 * Canonical home: this repository (xuansi-drivers). The firmware tree's
 * tools/wasm/hello.c is a frozen copy kept as build evidence of the ABI
 * freeze batch; further module edits happen here.
 *
 * Marshalling convention (abi=1, see README "Module ABI"): the host
 * serializes the caller's arguments as one msgpack array ->
 * xuansi_alloc -> writes it -> calls fn(ptr,len); fn returns a single
 * u64 = (result_ptr << 32) | result_len (C has no multi-value returns —
 * the (ptr,len) pair rides packed in one wasm i64) pointing at the
 * module's own msgpack result in linear memory.
 *
 * Exports (the full host-surface tour):
 *   xuansi_abi()    -> u32 abi version (host calls at load; != 1 -> refuse)
 *   xuansi_alloc(n) -> u32 offset (host writes args there; 0 = refuse)
 *   echo(ptr,len)   -> returns the pair unchanged (byte-level msgpack
 *                      round-trip)
 *   add(ptr,len)    -> decodes [a,b], returns [a+b] (module-side
 *                      decode+encode proof)
 *   gpio_echo(...)  -> args optional (default out=1/in=2): set 1 ->
 *                      read in, set 0 -> read again -> returns [v1, v0]
 *   now / now2x     -> millis (out-param convention) snapshots
 *   timed_delay     -> millis -> delay_ms(120) -> millis, returns [dt_ms]
 *   delay_wrap      -> delay(100) then delay(0xFFFFFFF8): pins the host
 *                      budget-cap refusal (no u32 wrap-around bypass)
 *   stream_out      -> stream_send an 8-byte sample -> returns [sent]
 *   spi_selftest    -> spi_config (loopback 12/13/14, 1MHz, mode0,
 *                      CS-less) + spi_xfer "XUANSI!?" -> returns
 *                      [rx==tx] (full-duplex loopback)
 *   spi_neg         -> spi_config(no_cs=0) -> raises (abi=1 has no CS
 *                      bus; negative proof of the host exception face)
 *   cam_frame       -> camera_info + camera_frame_get into a QVGA
 *                      frame buffer -> returns [w, h, len] (the camera
 *                      must already be on — caller's responsibility)
 */
#include <stdint.h>
#include <stddef.h>

#include "xuansi_host.h"

#ifndef XUANSI_ABI_OVERRIDE
#define XUANSI_ABI_OVERRIDE XUANSI_ABI
#endif

/* ---- native-test seam ------------------------------------------------
 * Unit tests (test_hello.c) compile this file with the host toolchain
 * against a stub host. The test owns a "linear memory" arena and passes
 * u32 offsets exactly like the wasm host does, so every pointer/offset
 * exchange below routes through XUANSI_IN/XUANSI_OFF and the module's
 * writable buffers live at fixed arena offsets. The wasm32 build
 * pre-processes every block in this seam away and compiles the original
 * expressions byte-for-byte. */

#ifdef XUANSI_NATIVE_TEST
uint8_t *xuansi_lin_base;                  /* test-owned linear-memory base */
#define XUANSI_IN(off) ((const uint8_t *)((size_t)xuansi_lin_base + (size_t)(uint32_t)(off)))
#define XUANSI_OFF(p)  ((uint32_t)((size_t)(p) - (size_t)xuansi_lin_base))
#define XUANSI_EXPORT(name) /* native: plain symbol, no wasm attribute */
#define LIN_OUT_OFF    0x4000u             /* result marshal buffer */
#define LIN_ARGS_OFF   0x6300u             /* reused args slot (fix1) */
#define LIN_FRAME_OFF  0x10000u            /* QVGA frame buffer */
#define LIN_TX_OFF     0x6000u             /* spi_selftest scratch */
#define LIN_RX_OFF     0x6100u
#define LIN_SAMP_OFF   0x6200u             /* stream_out sample scratch */
#define LIN_HEAP_OFF   0x6500u             /* bump arena (P3-T2 rider:
                                            * alloc returns heap_buf+off) */
void xuansi_native_init(uint8_t *base);
#else
#define XUANSI_IN(off) ((const uint8_t *)(uintptr_t)(off))
#define XUANSI_OFF(p)  ((uint32_t)(uintptr_t)(p))
#define XUANSI_EXPORT(name) __attribute__((export_name(name)))
#endif

/* ---- in-linear-memory marshal buffers (bump allocator; offset 0 is the
 * reserved failure sentinel) ----
 * fix1 args-slot convention (module-ABI.md §3.1): the host calls
 * xuansi_alloc on EVERY call-with-args — bump-only is a hidden per-call
 * budget (~230 calls at small-args scale). Small args (<= ARGS_SLOT_SZ)
 * reuse one fixed slot; safe because the fw host serializes every wasm
 * call (s_wasm_mu). Larger allocations keep the bump arena. hello is the
 * template new modules copy — the convention ships here on purpose. */

#define HEAP_SZ 8192
#define ARGS_SLOT_SZ 128
/* Linear-memory anchor: the bump allocator hands out heap_buf-BASED
 * addresses (P3-T2 rider, jpge@748506a's fix applied): the historical
 * code returned BARE arena offsets 8..8192 — linear addresses in
 * .rodata/.data territory, and heap_buf itself was optimized away
 * (unreferenced), so a >128B args write would stomp module state
 * through a bounds-valid pointer (the fw host validates range, not
 * ownership). No hello face passes >128B args today; the gun is closed
 * anyway — hello is the template new modules copy, so the SAFE
 * convention ships here on purpose. */
#ifdef XUANSI_NATIVE_TEST
/* native seam: the test arena backs the span; no anchor needed */
static uint32_t heap_off = 8;      /* 0 must never be returned (bump from 8) */
static uint8_t *args_slot;         /* placed by xuansi_native_init() */
static uint8_t *heap_buf;          /* placed by xuansi_native_init() */
#else
static uint8_t heap_buf[HEAP_SZ] __attribute__((aligned(8)));
static uint8_t args_slot[ARGS_SLOT_SZ] __attribute__((aligned(8)));
static uint32_t heap_off = 8;      /* 0 must never be returned (bump from 8) */
#endif

/* QVGA RGB565 frame buffer (cam_frame — 320*240*2 = 153,600B; linear
 * .bss, costs no module file size). Builds pin --initial-memory >=256KB. */
#define FRAME_CAP (320u * 240u * 2u)
#ifdef XUANSI_NATIVE_TEST
static uint8_t *frame_buf;                 /* placed by xuansi_native_init() */
#else
static uint8_t frame_buf[FRAME_CAP] __attribute__((aligned(8)));
#endif

XUANSI_EXPORT("xuansi_alloc")
uint32_t xuansi_alloc(uint32_t n)
{
    if (n == 0 || n > HEAP_SZ) return 0;
    if (n <= ARGS_SLOT_SZ) return XUANSI_OFF(args_slot);   /* fix1: reuse */
    uint32_t off = (heap_off + 7u) & ~(uint32_t)7u;
    if (off + n > HEAP_SZ) return 0;
    heap_off = off + n;
    return XUANSI_OFF(heap_buf) + off;   /* P3-T2 rider: heap_buf-based */
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

/* ---- minimal msgpack face (modules only need the int/array subset) ----
 * Decode: fixint / uint8..32 family; encode: fixarray + fixint (<128) /
 * uint16 / uint32. Enough for a hello-scale module; richer marshalling
 * is each module's own choice. */

static int mp_read_int(const uint8_t *p, uint32_t len, uint32_t *pos,
                       int64_t *out)
{
    if (*pos >= len) return -1;
    uint8_t t = p[(*pos)++];
    if ((t & 0x80) == 0) { *out = t; return 0; }             /* pos fixint */
    if (t >= 0xe0) { *out = (int8_t)t; return 0; }           /* neg fixint */
    uint32_t n = 0;
    if (t == 0xcc) n = 1;
    else if (t == 0xcd) n = 2;
    else if (t == 0xce) n = 4;
    else return -1;                                          /* out of domain (frozen value domain) */
    if (*pos + n > len) return -1;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = (v << 8) | p[(*pos)++];
    *out = (int64_t)v;
    return 0;
}

#ifdef XUANSI_NATIVE_TEST
static uint8_t *out_buf;                    /* placed by xuansi_native_init() */
#else
static uint8_t out_buf[512] __attribute__((aligned(8)));  /* result marshal buffer */
#endif

static uint32_t mp_put_int(uint8_t *p, uint32_t pos, uint64_t v)
{
    if (v < 128) { p[pos++] = (uint8_t)v; return pos; }
    if (v < 0x10000) {
        p[pos++] = 0xcd; p[pos++] = (uint8_t)(v >> 8); p[pos++] = (uint8_t)v;
        return pos;
    }
    p[pos++] = 0xce;
    p[pos++] = (uint8_t)(v >> 24); p[pos++] = (uint8_t)(v >> 16);
    p[pos++] = (uint8_t)(v >> 8);  p[pos++] = (uint8_t)v;
    return pos;
}

/* args subtree = one msgpack array; fetch integer #idx (out of domain or
 * missing returns dflt). */
static int64_t arg_int(const uint8_t *p, uint32_t len, uint32_t idx, int64_t dflt)
{
    uint32_t pos = 0;
    if (pos >= len) return dflt;
    uint8_t t = p[pos++];
    uint32_t n = 0;
    if ((t & 0xf0) == 0x90) n = t & 0x0f;
    else if (t == 0xdc) {
        if (pos + 2 > len) return dflt;
        n = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return dflt;
    int64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (mp_read_int(p, len, &pos, &v) != 0) return dflt;
        if (i == idx) return v;
    }
    return dflt;
}

/* ---- native seam init: place the writable buffers inside the arena ---- */
#ifdef XUANSI_NATIVE_TEST
void xuansi_native_init(uint8_t *base)
{
    xuansi_lin_base = base;
    out_buf = base + LIN_OUT_OFF;
    args_slot = base + LIN_ARGS_OFF;
    heap_buf = base + LIN_HEAP_OFF;
    frame_buf = base + LIN_FRAME_OFF;
}
#endif

/* ---- exported surface ---- */

XUANSI_EXPORT("echo")
uint64_t echo(uint32_t ptr, uint32_t len)
{
    return pack(ptr, len);
}

XUANSI_EXPORT("add")
uint64_t add(uint32_t ptr, uint32_t len)
{
    int64_t a = arg_int(XUANSI_IN(ptr), len, 0, 0);
    int64_t b = arg_int(XUANSI_IN(ptr), len, 1, 0);
    uint32_t pos = 0;
    out_buf[pos++] = 0x91;                      /* fixarray(1) */
    pos = mp_put_int(out_buf, pos, (uint64_t)(a + b));
    return pack(XUANSI_OFF(out_buf), pos);
}

XUANSI_EXPORT("gpio_echo")
uint64_t gpio_echo(uint32_t ptr, uint32_t len)
{
    uint32_t out_pin = (uint32_t)arg_int(XUANSI_IN(ptr), len, 0, 1);
    uint32_t in_pin = (uint32_t)arg_int(XUANSI_IN(ptr), len, 1, 2);
    static uint32_t g_hi, g_lo;
    gpio_set(out_pin, 1);
    gpio_get(in_pin, &g_hi);
    gpio_set(out_pin, 0);
    gpio_get(in_pin, &g_lo);
    uint32_t hi = g_hi, lo = g_lo;
    uint32_t pos = 0;
    out_buf[pos++] = 0x92;                      /* fixarray(2) */
    pos = mp_put_int(out_buf, pos, hi);
    pos = mp_put_int(out_buf, pos, lo);
    return pack(XUANSI_OFF(out_buf), pos);
}

static uint32_t g_t0, g_t1, g_t2;   /* out-param landing spots: linear .bss
                                      * (abi=1 frozen note — wasm stack slots
                                      * are on the execution page, out of the
                                      * host's write reach) */

XUANSI_EXPORT("now")
uint64_t now(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    uint32_t pos = 0;
    millis(&g_t0);
    out_buf[pos++] = 0x91;
    pos = mp_put_int(out_buf, pos, g_t0);
    return pack(XUANSI_OFF(out_buf), pos);
}

XUANSI_EXPORT("now2x")
uint64_t now2x(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    millis(&g_t0);
    millis(&g_t1);
    millis(&g_t2);
    uint32_t pos = 0;
    out_buf[pos++] = 0x93;
    pos = mp_put_int(out_buf, pos, g_t0);
    pos = mp_put_int(out_buf, pos, g_t1);
    pos = mp_put_int(out_buf, pos, g_t2);
    return pack(XUANSI_OFF(out_buf), pos);
}

XUANSI_EXPORT("timed_delay")
uint64_t timed_delay(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    millis(&g_t0);
    delay_ms(120);
    millis(&g_t1);
    uint32_t pos = 0;
    out_buf[pos++] = 0x91;
    pos = mp_put_int(out_buf, pos, g_t1 - g_t0);
    return pack(XUANSI_OFF(out_buf), pos);
}

/* budget-cap coverage vector: acc=100 then delay(u32 huge) — an old
 * additive check would wrap 100+0xFFFFFFF8 into a small value and hang
 * ~49.7 days; the subtractive check refuses. */
XUANSI_EXPORT("delay_wrap")
uint64_t delay_wrap(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    delay_ms(100);              /* acc=100 */
    delay_ms(0xFFFFFFF8u);      /* old: 100+0xFFFFFFF8 wraps -> passes; new: refuses */
    uint32_t pos = 0;           /* (unreachable on current fw — the refusal raises first) */
    out_buf[pos++] = 0x91;
    pos = mp_put_int(out_buf, pos, 1);
    return pack(XUANSI_OFF(out_buf), pos);
}

XUANSI_EXPORT("stream_out")
uint64_t stream_out(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    static const uint8_t sample[8] = { 'W', 'A', 'S', 'M', '!', 0xC2, 0xA1, 0x02 };
#ifdef XUANSI_NATIVE_TEST
    /* native seam: the stub host reads the buffer through the arena, so
     * stage the sample at a linear offset first */
    uint8_t *samp = xuansi_lin_base + LIN_SAMP_OFF;
    for (uint32_t i = 0; i < sizeof sample; i++) samp[i] = sample[i];
    uint32_t sent = 0;
    stream_send(&sent, XUANSI_OFF(samp), sizeof sample);
#else
    uint32_t sent = 0;
    stream_send(&sent, XUANSI_OFF(sample), sizeof sample);
#endif
    uint32_t pos = 0;
    out_buf[pos++] = 0x91;
    pos = mp_put_int(out_buf, pos, sent);
    return pack(XUANSI_OFF(out_buf), pos);
}

/* SPI loopback (bench wiring MOSI=12<->MISO=13, sclk=14 — same as the
 * bench.spi face): the module configures the bus itself and checks an
 * 8-byte full-duplex transfer, returning [rx==tx]. */
XUANSI_EXPORT("spi_selftest")
uint64_t spi_selftest(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    uint32_t rc = 7;
    spi_config(&rc, 0u, 14u, 1000000u, 12, 13, 0u, 1u);
    if (rc != 0) {
        out_buf[0] = 0x91; out_buf[1] = 0xe1;   /* [-1]: config refused */
        return pack(XUANSI_OFF(out_buf), 2);
    }
#ifdef XUANSI_NATIVE_TEST
    /* native seam: stage the loopback buffers at linear offsets */
    static const uint8_t lit[8] = { 'X','U','A','N','S','I','!','?' };
    uint8_t *tx = xuansi_lin_base + LIN_TX_OFF;
    uint8_t *rx = xuansi_lin_base + LIN_RX_OFF;
    for (uint32_t i = 0; i < 8; i++) { tx[i] = lit[i]; rx[i] = 0; }
#else
    static const uint8_t tx[8] = "XUANSI!?";
    static uint8_t rx[8] __attribute__((unused));
#endif
    uint32_t n = 0;
    spi_xfer(&n, 0u, XUANSI_OFF(tx), 8u, XUANSI_OFF(rx), 8u);
    uint32_t ok = (n == 8);
    for (uint32_t i = 0; i < n && i < 8; i++) {
        if (rx[i] != tx[i]) { ok = 0; break; }
    }
    uint32_t pos = 0;
    out_buf[pos++] = 0x91;
    pos = mp_put_int(out_buf, pos, ok);
    return pack(XUANSI_OFF(out_buf), pos);
}

/* Negative path: no_cs=0 (a CS bus) is refused on abi=1 — the host
 * surfaces it as a wasm exception (caller-visible class=rt). This
 * function does not return. */
XUANSI_EXPORT("spi_neg")
uint64_t spi_neg(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    uint32_t rc_neg = 7;
    spi_config(&rc_neg, 0u, 14u, 1000000u, 12, 13, 0u, 0u);
    (void)rc_neg;
    out_buf[0] = 0x91; out_buf[1] = 0;          /* [0] (unreachable) */
    return pack(XUANSI_OFF(out_buf), 2);
}

/* One camera frame (caller's responsibility: camera on first — the HIL
 * brings it up at QVGA). camera_info probe + frame_get into the QVGA
 * frame buffer -> [w, h, len]. */
XUANSI_EXPORT("cam_frame")
uint64_t cam_frame(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    static uint32_t info[3];
    camera_info(info);
    uint32_t flen = 0;
    camera_frame_get(&flen, XUANSI_OFF(frame_buf), FRAME_CAP);
    uint32_t pos = 0;
    out_buf[pos++] = 0x93;                      /* fixarray(3) */
    pos = mp_put_int(out_buf, pos, info[0]);
    pos = mp_put_int(out_buf, pos, info[1]);
    pos = mp_put_int(out_buf, pos, flen);
    return pack(XUANSI_OFF(out_buf), pos);
}
