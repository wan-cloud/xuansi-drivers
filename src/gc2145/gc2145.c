/*
 * gc2145.c — GC2145 DVP camera sensor-profile module (kind=
 * sensor-profile; B12-T5 — the "driver lives in wasm" thesis:
 * the whole bring-up sequence is module data + module code, the host
 * only supplies the raw i2c/cam natives).
 *
 * Canonical home: wan-cloud/xuansi-drivers (src/gc2145/gc2145.c).
 *
 * Board facts: GC2145 on the bench camera epoch — SCCB 7-bit addr
 * 0x3C on i2c slot 0 pins SDA=4 SCL=5 @ 100kHz, PID 0x2145 at regs
 * 0xF0/0xF1, XCLK 20MHz out of pin 15 (the cam engine's direct LCD_CAM
 * drive). Register knowledge: see gc2145_regs.h (transcribed from the
 * vendored espressif/esp32-camera 2.1.8 driver, Apache-2.0).
 *
 * Calling face (module ABI marshalling — one msgpack array in, one
 * msgpack map result out; the recarved ABI 1 unified table):
 *
 *   bench.wasm.call("gc2145", "init", {framesize="QVGA"|"VGA"?})
 *     framesize optional, default "QVGA"; an unknown framesize string
 *     is an honest in-domain error BEFORE any hardware touch.
 *     Sequence (each step a host native): if a stream is running,
 *     cam_stop + >=1000ms settle; cam_xclk(20MHz); 2000ms warmup in
 *     100ms slices (GC2145 SCCB needs 1-2s after the clock starts —
 *     host template note); i2c_config(0, 4, 5, 100k); PID probe via
 *     i2c_writeread {0xF0} then {0xF1} (mismatch -> error carrying
 *     the READ pid); SOFT RESET FIRST — reg 0xFE <- 0xE0, 100ms —
 *     then the full 582-pair init table, 100ms, the per-framesize
 *     geometry deltas, the RGB565 format regs, cam_start(w,h,0).
 *     -> {ok=1, sensor="gc2145", w, h, ms}
 *
 *     The soft reset is first-class module knowledge (source-level
 *     diagnosis of the vendored driver: its camera_probe->reset()
 *     path hides the write; the vendored reset() is reset-then-table,
 *     and both init AND every framesize re-init run it explicitly).
 *
 *   bench.wasm.call("gc2145", "off")
 *     cam_stop (host idempotent: stop stream, release the 11 DVP +
 *     XCLK pin claims) + module marks itself off. Safe to repeat.
 *     -> {ok=1, on=0, ms}
 *
 *   bench.wasm.call("gc2145", "info")
 *     cam_frame_info passthrough + module state. framesize is the
 *     last requested one (kept across off; absent before the first
 *     init).
 *     -> {sensor="gc2145", framesize?, w, h, pixfmt, on}
 *
 * Error discipline: in-domain refusals are {error=...} maps (unknown
 * framesize; PID mismatch additionally carries got/want ints and the
 * message embeds the read value). Host-level failures (SCCB NACK,
 * slot conflict, cam floor) surface as the host's wasm exception —
 * carried VERBATIM, never swallowed into an error map.
 *
 * Decisions (documented per the brief's open choices):
 *   - i2c batching: PER-REGISTER i2c_write (582 x 2B transactions).
 *     The packed 1164B single write would fit the 4096B engine cap,
 *     but the GC2145 SCCB interface has no documented multi-register
 *     auto-increment — the vendored driver also writes every pair as
 *     its own SCCB_Write. Wire-faithful beats wire-cheap (~150ms at
 *     100kHz).
 *   - warmup/settle slicing: 100ms delay_ms slices (20x for the 2s
 *     warmup, 10x for the 1s settle). The host slices internally
 *     anyway; the uniform 100ms step keeps the module's own delay
 *     accounting simple and matches the vendored reset() delays.
 *
 * Memory: STATIC ONLY (the instance heap_size=0 lesson — WAMR builtin
 * malloc is unavailable). Table in .rodata, args-slot reuse per
 * module-ABI.md §3.1 (allocs <=128B share one fixed slot; the fw host
 * serializes every wasm call so the reuse is race-free). out 2048 +
 * heap 512 + args 128 + io 4 + table 1164 ~= 3.9KB -> the frozen
 * default 262144B initial (4 pages; the B15 ABI 1 freeze pins
 * max == initial).
 */

#include <stdint.h>
#include <stddef.h>

/* B15 ABI recarve (ruled 2026-11-11): the ABI number is 1 AGAIN — the
 * clean recarve; the old abi=1/abi=2 are retired with fw 0.5.0. This
 * module's import face (the i2c + cam families) is fully inside the
 * recarved UNIFIED table — the old abi=2 additions were folded in —
 * so the declaration drops the XUANSI_ABI=2 override and stays at the
 * header default 1; what CHANGES for the recarved lane is the
 * linear-memory freeze: initial == max (the static-footprint
 * admission contract), i.e. build.sh pins --initial-memory=262144
 * --max-memory=262144 (the pre-recarve artifact carried max=1MB and
 * is honestly refused by the fw 0.5.0 initial==max gate — retired
 * with the wave-3 purge). */
#include "xuansi_host.h"
#include "gc2145_regs.h"

#ifndef XUANSI_ABI_OVERRIDE
#define XUANSI_ABI_OVERRIDE XUANSI_ABI
#endif

/* ---- native-test seam (same discipline as hello/ledwatch/st7789) ----
 * Unit tests compile this file with the host toolchain against a stub
 * host (fake i2c + fake cam engine recording every transaction). The
 * test owns the "linear memory" arena; every host-touched buffer lives
 * at a fixed arena offset. The wasm32 build pre-processes the seam
 * away. */

#ifdef XUANSI_NATIVE_TEST
uint8_t *xuansi_lin_base;
#define XUANSI_IN(off) ((const uint8_t *)((size_t)xuansi_lin_base + (size_t)(uint32_t)(off)))
#define XUANSI_OFF(p)  ((uint32_t)((size_t)(p) - (size_t)xuansi_lin_base))
#define XUANSI_EXPORT(name) /* native: plain symbol, no wasm attribute */
#define LIN_OUT_OFF    0x4000u             /* result marshal buffer */
#define LIN_ARGS_OFF   0x6000u             /* reused args slot (§3.1) */
#define LIN_IO_OFF     0x6080u             /* 4B i2c write/probe io */
#define LIN_HEAP_OFF   0x6200u             /* bump arena (P3-T12 rider:
                                            * alloc returns heap_buf+off) */
void xuansi_native_init(uint8_t *base);
#else
#define XUANSI_IN(off) ((const uint8_t *)(uintptr_t)(off))
#define XUANSI_OFF(p)  ((uint32_t)(uintptr_t)(p))
#define XUANSI_EXPORT(name) __attribute__((export_name(name)))
#endif

/* ---- in-linear-memory buffers (offset 0 reserved; §3.1 slot reuse) ---- */

#define HEAP_SZ 512
#define ARGS_SLOT_SZ 128
#ifdef XUANSI_NATIVE_TEST
static uint32_t heap_off = 8;
static uint8_t *args_slot;                 /* placed by xuansi_native_init() */
static uint8_t *heap_buf;                  /* placed by xuansi_native_init() */
static uint8_t *io_buf;                    /* 4B: write[0..1] / read[2] */
static uint8_t *out_buf;                   /* result marshal buffer */
#else
/* P3-T12 rider (T2's routed item, hello/ledwatch/jpge's fix applied):
 * heap_buf is REFERENCED now — the bump path returns heap_buf-based
 * addresses, not the bare offsets 8..512 it used to hand out (linear
 * addresses in .rodata/.data territory — and the unreferenced array
 * was optimized away, so those offsets pointed at real module state,
 * the register table included). No gc2145 face passes >128B args
 * today; the loaded gun is closed anyway. */
static uint8_t heap_buf[HEAP_SZ] __attribute__((aligned(8)));
static uint8_t args_slot[ARGS_SLOT_SZ] __attribute__((aligned(8)));
static uint8_t io_buf[4] __attribute__((aligned(8)));
static uint8_t out_buf[2048] __attribute__((aligned(8)));
static uint32_t heap_off = 8;
#endif

/* module state */
static int st_on;                          /* a stream WE started is on */
static int st_fs;                          /* 0 none / 1 QVGA / 2 VGA */

/* host-out landing spots (linear .bss — the host writes them; wasm
 * stack slots sit on the execution page, out of the host's reach: §2.1) */
static uint32_t g_rc, g_w, g_h, g_pf, g_on, g_t0, g_t1;

XUANSI_EXPORT("xuansi_alloc")
uint32_t xuansi_alloc(uint32_t n)
{
    if (n == 0 || n > HEAP_SZ) return 0;
    if (n <= ARGS_SLOT_SZ) return XUANSI_OFF(args_slot);   /* §3.1 reuse */
    uint32_t off = (heap_off + 7u) & ~(uint32_t)7u;
    if (off + n > HEAP_SZ) return 0;
    heap_off = off + n;
    return XUANSI_OFF(heap_buf) + off;   /* P3-T12 rider: heap_buf-based */
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

/* ---- minimal msgpack face (int encode; fixstr encode; fixmap) ---- */

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

static uint32_t mp_put_str(uint8_t *p, uint32_t pos, const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    p[pos++] = (uint8_t)(0xa0 | n);               /* fixstr (n <= 31) */
    for (uint32_t i = 0; i < n; i++) p[pos++] = (uint8_t)s[i];
    return pos;
}

typedef struct {
    uint32_t pos;
    uint32_t nkv;
} enc_t;

static void enc_begin(enc_t *e)
{
    e->pos = 1;                 /* fixmap count patched at end */
    e->nkv = 0;
}

static void enc_int(enc_t *e, const char *k, uint64_t v)
{
    e->pos = mp_put_str(out_buf, e->pos, k);
    e->pos = mp_put_int(out_buf, e->pos, v);
    e->nkv++;
}

static void enc_str(enc_t *e, const char *k, const char *v)
{
    e->pos = mp_put_str(out_buf, e->pos, k);
    e->pos = mp_put_str(out_buf, e->pos, v);
    e->nkv++;
}

static uint64_t enc_end(enc_t *e)
{
    out_buf[0] = (uint8_t)(0x80 | e->nkv);           /* nkv <= 15 */
    return pack(XUANSI_OFF(out_buf), e->pos);
}

static uint64_t err_result(const char *msg)
{
    enc_t e;
    enc_begin(&e);
    enc_str(&e, "error", msg);
    return enc_end(&e);
}

/* ---- framesize arg decode (tolerant defaults; honest bad value) ----
 * args[0] = fixarray[ fixmap{ framesize: fixstr|str8 } ] — the shape
 * bench.wasm.call marshals. Absent/malformed input keeps the QVGA
 * default (st7789's tolerant-cfg discipline); an explicit framesize
 * string that is neither QVGA nor VGA is an honest refusal. */

static int key_is(const char *key, uint32_t klen, const char *lit)
{
    uint32_t i = 0;
    while (i < klen && lit[i] && key[i] == lit[i]) i++;
    return i == klen && lit[i] == 0;
}

/* returns 0 QVGA, 1 VGA, -1 unknown framesize */
static int decode_fs(const uint8_t *p, uint32_t len)
{
    if (p == NULL || len == 0) return 0;
    uint32_t pos = 0;
    uint8_t t = p[pos++];
    uint32_t n;
    if ((t & 0xf0) == 0x90) n = t & 0x0f;            /* fixarray */
    else if (t == 0xdc) {                            /* array16 */
        if (pos + 2 > len) return 0;
        n = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return 0;
    if (n < 1) return 0;
    if (pos >= len) return 0;
    t = p[pos++];
    if ((t & 0xf0) == 0x80) n = t & 0x0f;            /* fixmap */
    else if (t == 0xde) {                            /* map16 */
        if (pos + 2 > len) return 0;
        n = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return 0;

    for (uint32_t e = 0; e < n; e++) {
        if (pos >= len) return 0;
        uint8_t kt = p[pos];
        if ((kt & 0xe0) != 0xa0) return 0;           /* key: fixstr */
        uint32_t klen = kt & 0x1f;
        if (pos + 1 + klen > len) return 0;
        char key[16];
        if (klen >= sizeof key) { pos += 1 + klen; continue; }
        for (uint32_t i = 0; i < klen; i++) key[i] = (char)p[pos + 1 + i];
        key[klen] = 0;
        pos += 1 + klen;
        if (pos >= len) return 0;

        uint8_t vt = p[pos];
        if ((vt & 0xe0) == 0xa0 || vt == 0xd9) {     /* str value */
            uint32_t vlen;
            if ((vt & 0xe0) == 0xa0) { vlen = vt & 0x1f; pos++; }
            else {
                if (pos + 1 >= len) return 0;
                vlen = p[pos + 1]; pos += 2;
            }
            if (pos + vlen > len) return 0;
            if (key_is(key, klen, "framesize")) {
                if (vlen == 4 && p[pos]=='Q' && p[pos+1]=='V'
                              && p[pos+2]=='G' && p[pos+3]=='A') return 0;
                if (vlen == 3 && p[pos]=='V' && p[pos+1]=='G'
                              && p[pos+2]=='A') return 1;
                return -1;                           /* explicit bad value */
            }
            pos += vlen;
        } else if (vt == 0xc3 || vt == 0xc2) {       /* bool: skip */
            pos++;
        } else if (vt < 0x80 || vt >= 0xe0
                   || vt == 0xcc || vt == 0xcd || vt == 0xce) {
            /* int: skip by width */
            uint32_t q = pos;
            if (vt < 0x80 || vt >= 0xe0) q++;
            else if (vt == 0xcc) q += 2;
            else if (vt == 0xcd) q += 3;
            else q += 5;
            if (q > len) return 0;
            pos = q;
        } else {
            return 0;                                /* other types: default */
        }
    }
    return 0;
}

/* ---- register helpers (the whole SCCB register face) ---- */

/* one register write: its own i2c_write transaction (see the file
 * header's batching decision — wire-faithful per-pair delivery) */
static void write_reg(uint8_t reg, uint8_t val)
{
    io_buf[0] = reg;
    io_buf[1] = val;
    g_rc = 0;
    i2c_write(&g_rc, GC2145_SCCB_SLOT, GC2145_SCCB_ADDR,
              XUANSI_OFF(io_buf), 2u);
}

static void write_table(const uint8_t (*regs)[2], uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        write_reg(regs[i][0], regs[i][1]);
}

/* register-addressed read, atomic form (repeated start — the same
 * shape as the vendored SCCB_Read) */
static uint8_t read_reg(uint8_t reg)
{
    io_buf[0] = reg;
    g_rc = 0;
    i2c_writeread(&g_rc, GC2145_SCCB_SLOT, GC2145_SCCB_ADDR,
                  XUANSI_OFF(io_buf), 1u, XUANSI_OFF(io_buf) + 2u, 1u);
    return io_buf[2];
}

/* "0x%04x" into dst (7 bytes) — the error message carries the READ
 * pid (no libc printf in a -nostdlib module) */
static void put_hex16(char *dst, uint32_t v)
{
    dst[0] = '0'; dst[1] = 'x';
    for (int i = 0; i < 4; i++) {
        uint32_t nib = (v >> (12 - 4 * i)) & 0xFu;
        dst[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    dst[6] = 0;
}

/* ---- the bring-up sequence, shared by init and framesize re-init ----
 * The caller has already stopped any running stream and settled. */

static uint64_t bring_up(int fs)
{
    uint32_t w = fs == 1 ? 640u : 320u;     /* fs: 0 QVGA / 1 VGA */
    uint32_t h = fs == 1 ? 480u : 240u;

    /* XCLK, then the 2s SCCB warmup (100ms slices) */
    g_rc = 0;
    cam_xclk(&g_rc, GC2145_XCLK_HZ);
    for (uint32_t i = 0; i < GC2145_WARMUP_MS; i += 100u) delay_ms(100u);

    /* SCCB bus */
    g_rc = 0;
    i2c_config(&g_rc, GC2145_SCCB_SLOT, GC2145_SCCB_SDA,
               GC2145_SCCB_SCL, GC2145_SCCB_HZ);

    /* PID probe: hi then lo, atomic register reads — two locals so the
     * hi-before-lo wire order (what the tests pin) is a language
     * guarantee, not an accident of expression sequencing */
    uint32_t pid_hi = read_reg(GC2145_CHIP_ID_HIGH);
    uint32_t pid_lo = read_reg(GC2145_CHIP_ID_LOW);
    uint32_t pid = (pid_hi << 8) | pid_lo;
    if (pid != GC2145_PID) {
        char msg[48] = "pid mismatch: got ";
        put_hex16(msg + 18, pid);
        enc_t e;
        enc_begin(&e);
        enc_str(&e, "error", msg);
        enc_int(&e, "got", pid);
        enc_int(&e, "want", GC2145_PID);
        return enc_end(&e);
    }

    /* SOFTWARE RESET FIRST — explicit first-class knowledge (the
     * vendored camera_probe->reset() path hides this write; the
     * vendored reset() is soft-reset, 100ms, table, 100ms) */
    write_reg(GC2145_RESET_RELATED, GC2145_SOFT_RESET_VAL);
    delay_ms(GC2145_RESET_DELAY_MS);

    write_table(gc2145_init_regs, GC2145_INIT_REGS_N);
    delay_ms(GC2145_POST_TABLE_DELAY_MS);

    write_table(fs == 1 ? gc2145_fs_vga_regs : gc2145_fs_qvga_regs,
                GC2145_FS_REGS_N);
    write_table(gc2145_rgb565_regs, 2u);

    /* capture on — geometry decided by this profile, RGB565 (pixfmt 0,
     * this wave's only format) */
    g_rc = 0;
    cam_start(&g_rc, w, h, 0u);
    st_on = 1;
    st_fs = fs + 1;                          /* 1 QVGA / 2 VGA */

    enc_t e;
    enc_begin(&e);
    enc_int(&e, "ok", 1);
    enc_str(&e, "sensor", "gc2145");
    enc_int(&e, "w", w);
    enc_int(&e, "h", h);
    millis(&g_t1);
    enc_int(&e, "ms", g_t1 - g_t0);
    return enc_end(&e);
}

/* ---- exported face ---- */

XUANSI_EXPORT("init")
uint64_t gc_init(uint32_t ptr, uint32_t len)
{
    int fs = decode_fs(XUANSI_IN(ptr), len);
    if (fs < 0) return err_result("unknown framesize");

    millis(&g_t0);
    if (st_on) {
        /* framesize switch (or explicit re-init): full off, then the
         * >=1000ms settle the sensor wants after cam_stop */
        g_rc = 0;
        cam_stop(&g_rc);
        st_on = 0;
        for (uint32_t i = 0; i < GC2145_SETTLE_MS; i += 100u) delay_ms(100u);
    }
    return bring_up(fs);
}

XUANSI_EXPORT("off")
uint64_t gc_off(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    millis(&g_t0);
    g_rc = 0;
    cam_stop(&g_rc);                         /* host idempotent */
    st_on = 0;
    millis(&g_t1);

    enc_t e;
    enc_begin(&e);
    enc_int(&e, "ok", 1);
    enc_int(&e, "on", 0);
    enc_int(&e, "ms", g_t1 - g_t0);
    return enc_end(&e);
}

XUANSI_EXPORT("info")
uint64_t gc_info(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    cam_frame_info(&g_w, &g_h, &g_pf, &g_on);

    enc_t e;
    enc_begin(&e);
    enc_str(&e, "sensor", "gc2145");
    if (st_fs) enc_str(&e, "framesize", st_fs == 2 ? "VGA" : "QVGA");
    enc_int(&e, "w", g_w);
    enc_int(&e, "h", g_h);
    enc_int(&e, "pixfmt", g_pf);
    enc_int(&e, "on", g_on);
    return enc_end(&e);
}

/* ---- native seam init: place the host-touched buffers in the arena ---- */
#ifdef XUANSI_NATIVE_TEST
void xuansi_native_init(uint8_t *base)
{
    xuansi_lin_base = base;
    out_buf = base + LIN_OUT_OFF;
    args_slot = base + LIN_ARGS_OFF;
    heap_buf = base + LIN_HEAP_OFF;
    io_buf = base + LIN_IO_OFF;
}
#endif
