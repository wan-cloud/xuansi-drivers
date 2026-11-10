/*
 * test_gc2145.c — native unit test for the gc2145 sensor-profile module
 * (stub host; B12-T5).
 *
 * Build & run (host toolchain, no wasm needed):
 *   gcc -DXUANSI_NATIVE_TEST -Wall -Wextra -Werror -I src -I src/gc2145 \
 *      src/gc2145/test_gc2145.c -o build/test_gc2145 \
 *      && ./build/test_gc2145
 *
 * Same seam as test_ledwatch.c / test_st7789.c: the test includes
 * gc2145.c directly (one translation unit), provides the `xuansi`
 * host namespace as stubs, and drives init/off/info through the same
 * u32-offset msgpack marshalling the wasm host uses — the test owns
 * the "linear memory" arena via xuansi_native_init().
 *
 * The stub host models:
 *   - the fake I2C/SCCB bus: a programmable PID served on register
 *     reads (0xF0/0xF1), and an optional programmed NACK at the Nth
 *     i2c_write (the trap channel, exercised verbatim);
 *   - the fake cam engine: xclk/start/stop recorded, frame_info
 *     serving the last started geometry;
 *   - a virtual clock advanced by delay_ms (millis serves it).
 * Every host call lands in one ordered event log the checks assert on
 * (probe order, soft-reset-first, warmup/settle accounting, the full
 * 582-pair init table pair-by-pair, framesize tails, cam_start
 * geometry).
 *
 * Independent transcription guard: the pair count (582) and FNV-1a32
 * (0x89b1a596) below were derived from the VENDORED esp32-camera
 * gc2145_settings.h, not from the transcribed array — any post-hoc
 * edit of gc2145_regs.h trips T10.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>

#include "gc2145.c"   /* unity build: the module under test */

/* ---- expected constants (derived from the vendored source tree) ---- */
#define EXP_TABLE_N   582u
#define EXP_TABLE_FNV 0x89b1a596u

/* ---- stub host: the ordered event log ---- */

enum {
    EV_XCLK = 'X', EV_CSTART = 'S', EV_CSTOP = 'T',   /* cam engine */
    EV_ICONFIG = 'C', EV_IWRITE = 'W', EV_IWREAD = 'R', /* i2c */
    EV_DELAY = 'D',
};

typedef struct {
    uint8_t kind;
    uint32_t a, b, c, d;      /* kind-specific args */
    uint8_t wbuf[8];          /* captured write payload (i2c) */
    uint32_t wlen;
    uint32_t ms;              /* delay_ms amount */
} ev_t;

#define EV_MAX 900
static ev_t s_ev[EV_MAX];
static int s_ev_n;

static void ev_reset(void) { s_ev_n = 0; }

static ev_t *ev_add(uint8_t kind)
{
    if (s_ev_n >= EV_MAX) {
        fprintf(stderr, "FATAL: event log overflow\n");
        exit(2);
    }
    ev_t *e = &s_ev[s_ev_n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    return e;
}

/* first event of a kind at or after `from` (NULL if none) */
static const ev_t *ev_find(uint8_t kind, int from)
{
    for (int i = from < 0 ? 0 : from; i < s_ev_n; i++)
        if (s_ev[i].kind == kind) return &s_ev[i];
    return NULL;
}

static int ev_idx(const ev_t *e) { return e ? (int)(e - s_ev) : -1; }

/* summed delay_ms between log positions (lo, hi) exclusive */
static uint32_t delay_sum_between(int lo, int hi)
{
    uint32_t s = 0;
    for (int i = lo + 1; i < hi && i < s_ev_n; i++)
        if (s_ev[i].kind == EV_DELAY) s += s_ev[i].ms;
    return s;
}

/* ---- stub host state ---- */

static uint16_t s_pid = 0x2145;       /* programmable sensor PID */
static int s_fail_at;                 /* 1-based i2c_write NACK index */
static int s_write_count;
static uint32_t s_virt_ms;            /* virtual clock */

static int s_cam_on, s_cam_w, s_cam_h, s_cam_pf;

/* trap emulation: the host refusal channel (same as test_ledwatch) */
static jmp_buf s_jmp;
static int     s_trap_armed;
static char    s_trap_msg[128];
static void stub_raise(const char *msg)
{
    if (!s_trap_armed) {
        fprintf(stderr, "FATAL: stub host raised outside an armed trap: %s\n", msg);
        exit(2);
    }
    snprintf(s_trap_msg, sizeof s_trap_msg, "%s", msg);
    longjmp(s_jmp, 1);
}

static void host_reset(void)
{
    ev_reset();
    s_fail_at = 0;
    s_write_count = 0;
}

/* ---- the `xuansi` namespace stubs (only what gc2145 imports) ---- */

void xuansi_millis(uint32_t *out) { *out = s_virt_ms; }

void xuansi_delay_ms(uint32_t ms)
{
    ev_t *e = ev_add(EV_DELAY);
    e->ms = ms;
    s_virt_ms += ms;
}

void xuansi_i2c_config(uint32_t *out, uint32_t slot, uint32_t sda,
                       uint32_t scl, uint32_t hz)
{
    ev_t *e = ev_add(EV_ICONFIG);
    e->a = slot; e->b = sda; e->c = scl; e->d = hz;
    *out = 0;
}

void xuansi_i2c_write(uint32_t *out, uint32_t slot, uint32_t addr,
                      uint32_t wbuf_off, uint32_t wlen)
{
    if (s_fail_at && ++s_write_count == s_fail_at)
        stub_raise("xuansi.i2c_write: nack (programmed)");
    ev_t *e = ev_add(EV_IWRITE);
    e->a = slot; e->b = addr;
    e->wlen = wlen;
    if (wlen > sizeof e->wbuf) wlen = sizeof e->wbuf;
    memcpy(e->wbuf, xuansi_lin_base + wbuf_off, wlen);
    *out = e->wlen;
}

void xuansi_i2c_writeread(uint32_t *out, uint32_t slot, uint32_t addr,
                          uint32_t wbuf_off, uint32_t wlen,
                          uint32_t rbuf_off, uint32_t rcap)
{
    ev_t *e = ev_add(EV_IWREAD);
    e->a = slot; e->b = addr;
    e->wlen = wlen;
    if (wlen > sizeof e->wbuf) wlen = sizeof e->wbuf;
    memcpy(e->wbuf, xuansi_lin_base + wbuf_off, wlen);
    /* the sensor model: PID registers only */
    uint8_t v = 0xFF;
    if (e->wbuf[0] == 0xF0) v = (uint8_t)(s_pid >> 8);
    else if (e->wbuf[0] == 0xF1) v = (uint8_t)(s_pid & 0xFF);
    if (rcap) { xuansi_lin_base[rbuf_off] = v; *out = 1; }
    else *out = 0;
}

void xuansi_cam_xclk(uint32_t *out, uint32_t hz)
{
    ev_t *e = ev_add(EV_XCLK);
    e->a = hz;
    *out = 0;
}

void xuansi_cam_start(uint32_t *out, uint32_t w, uint32_t h, uint32_t pixfmt)
{
    ev_t *e = ev_add(EV_CSTART);
    e->a = w; e->b = h; e->c = pixfmt;
    s_cam_on = 1; s_cam_w = (int)w; s_cam_h = (int)h; s_cam_pf = (int)pixfmt;
    *out = 0;
}

void xuansi_cam_stop(uint32_t *out)
{
    ev_add(EV_CSTOP);
    s_cam_on = 0;
    *out = 0;
}

void xuansi_cam_frame_info(uint32_t *w_out, uint32_t *h_out,
                           uint32_t *pixfmt_out, uint32_t *on_out)
{
    *w_out = s_cam_on ? (uint32_t)s_cam_w : 0;
    *h_out = s_cam_on ? (uint32_t)s_cam_h : 0;
    *pixfmt_out = s_cam_on ? (uint32_t)s_cam_pf : 0;
    *on_out = (uint32_t)s_cam_on;
}

/* ---- harness ---- */

static int checks, fails;
static void check(int ok, const char *name, const char *detail)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s (%s)\n", name, detail ? detail : "");
    } else {
        printf("PASS %s\n", name);
    }
}

/* the module's linear-memory arena (test-owned) */
static uint8_t arena[1u << 20] __attribute__((aligned(4096)));

/* ---- msgpack result decoding (independent of the module's encoder) ---- */

typedef struct {
    int has_error;
    char error[64];
    long ok, w, h, on, pixfmt, got, want, ms;
    char sensor[16], framesize[16];
    int has_sensor, has_framesize, has_got, has_want, has_w, has_h,
        has_on, has_pixfmt, has_ms, has_ok;
} result_t;

static const uint8_t *s_res_p;
static uint32_t s_res_len, s_res_pos;

static int rd_u8(uint8_t *out)
{
    if (s_res_pos >= s_res_len) return -1;
    *out = s_res_p[s_res_pos++];
    return 0;
}

static int rd_i64(int64_t *out)
{
    uint8_t t;
    if (rd_u8(&t) != 0) return -1;
    if ((t & 0x80) == 0) { *out = t; return 0; }
    if (t >= 0xe0) { *out = (int8_t)t; return 0; }
    uint32_t n = 0;
    if (t == 0xcc) n = 1;
    else if (t == 0xcd) n = 2;
    else if (t == 0xce) n = 4;
    else return -1;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b;
        if (rd_u8(&b) != 0) return -1;
        v = (v << 8) | b;
    }
    *out = (int64_t)v;
    return 0;
}

/* fixstr or str8 value into out (cap includes NUL room) */
static int rd_str(char *out, uint32_t cap)
{
    uint8_t t;
    if (rd_u8(&t) != 0) return -1;
    uint32_t n;
    if ((t & 0xe0) == 0xa0) n = t & 0x1f;
    else if (t == 0xd9) {
        if (rd_u8(&t) != 0) return -1;
        n = t;
    } else return -1;
    if (s_res_pos + n > s_res_len || n >= cap) return -1;
    memcpy(out, s_res_p + s_res_pos, n);
    out[n] = 0;
    s_res_pos += n;
    return (int)n;
}

static int rd_key(const char *want)
{
    uint32_t save = s_res_pos;
    char k[16];
    if (rd_str(k, sizeof k) != (int)strlen(want) || strcmp(k, want) != 0) {
        s_res_pos = save;
        return -1;
    }
    return 0;
}

static int res_decode(uint64_t packed, result_t *res)
{
    memset(res, 0, sizeof *res);
    s_res_p = arena + (uint32_t)(packed >> 32);
    s_res_len = (uint32_t)packed;
    s_res_pos = 0;
    uint8_t t;
    if (rd_u8(&t) != 0 || (t & 0xf0) != 0x80) return -1;   /* fixmap root */
    uint32_t nkv = t & 0x0f;
    for (uint32_t i = 0; i < nkv; i++) {
        if (rd_key("error") == 0) {
            res->has_error = 1;
            if (rd_str(res->error, sizeof res->error) < 0) return -1;
        } else if (rd_key("ok") == 0) {
            res->has_ok = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->ok = (long)v;
        } else if (rd_key("sensor") == 0) {
            res->has_sensor = 1;
            if (rd_str(res->sensor, sizeof res->sensor) < 0) return -1;
        } else if (rd_key("framesize") == 0) {
            res->has_framesize = 1;
            if (rd_str(res->framesize, sizeof res->framesize) < 0) return -1;
        } else if (rd_key("w") == 0) {
            res->has_w = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->w = (long)v;
        } else if (rd_key("h") == 0) {
            res->has_h = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->h = (long)v;
        } else if (rd_key("pixfmt") == 0) {
            res->has_pixfmt = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->pixfmt = (long)v;
        } else if (rd_key("on") == 0) {
            res->has_on = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->on = (long)v;
        } else if (rd_key("got") == 0) {
            res->has_got = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->got = (long)v;
        } else if (rd_key("want") == 0) {
            res->has_want = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->want = (long)v;
        } else if (rd_key("ms") == 0) {
            res->has_ms = 1;
            int64_t v; if (rd_i64(&v) != 0) return -1;
            res->ms = (long)v;
        } else {
            return -1;    /* unknown key: the face is frozen */
        }
    }
    return 0;
}

/* ---- the wasm host's arg-writing step: alloc, then write msgpack ---- */

static uint32_t host_write_args(const uint8_t *bytes, uint32_t n)
{
    uint32_t off = xuansi_alloc(n);
    if (off == 0) { fprintf(stderr, "FATAL: alloc refused\n"); exit(2); }
    memcpy(arena + off, bytes, n);
    return off;
}

/* args = fixarray(1)[ fixmap(1){ framesize: fixstr } ] — the shape
 * bench.wasm.call("gc2145","init",{framesize="QVGA"}) marshals */
static uint64_t call_init_fs(const char *fs)
{
    uint8_t abuf[64];
    uint32_t pos = 0;
    abuf[pos++] = 0x91;                       /* fixarray(1) */
    abuf[pos++] = 0x81;                       /* fixmap(1) */
    abuf[pos++] = (uint8_t)(0xa0 | 9);        /* fixstr(9) "framesize" */
    memcpy(abuf + pos, "framesize", 9); pos += 9;
    uint32_t n = (uint32_t)strlen(fs);
    abuf[pos++] = (uint8_t)(0xa0 | n);        /* fixstr value */
    memcpy(abuf + pos, fs, n); pos += n;
    uint32_t off = host_write_args(abuf, pos);
    return gc_init(off, pos);
}

static uint64_t call_init_noargs(void)
{
    return gc_init(0, 0);
}

static uint64_t call_off(void)  { return gc_off(0, 0); }
static uint64_t call_info(void) { return gc_info(0, 0); }

/* ---- FNV-1a32 over the transcribed table (T10) ---- */
static uint32_t table_fnv(void)
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < GC2145_INIT_REGS_N; i++)
        for (int j = 0; j < 2; j++) {
            h ^= gc2145_init_regs[i][j];
            h *= 0x01000193u;
        }
    return h;
}

/* collect indexes of all i2c_write events (W list) */
static int w_list(int *idx, int cap)
{
    int n = 0;
    for (int i = 0; i < s_ev_n && n < cap; i++)
        if (s_ev[i].kind == EV_IWRITE) idx[n++] = i;
    return n;
}

static char detail[256];

int main(void)
{
    xuansi_native_init(arena);
    host_reset();

    /* T01: abi marker — the loader's instantiate gate reads this */
    check(xuansi_abi() == 2, "T01 xuansi_abi()==2 (abi=2 module)", "");

    /* T02: args slot discipline (§3.1: <=128B allocs reuse one slot) */
    {
        uint32_t a = xuansi_alloc(24), b = xuansi_alloc(24);
        snprintf(detail, sizeof detail, "a=%u b=%u", a, b);
        check(a != 0 && a == b, "T02 args-slot reuse (§3.1)", detail);
        check(xuansi_alloc(0) == 0, "T02b alloc(0) refused", "");
        uint32_t c = xuansi_alloc(200);
        snprintf(detail, sizeof detail, "c=%u want=%u", c, LIN_HEAP_OFF + 8);
        check(c == LIN_HEAP_OFF + 8, "T02c large alloc -> heap_buf-based bump "
              "(P3-T12 rider: no bare .rodata-range offsets)", detail);
    }

    /* T03: info before any init — honest empty state */
    {
        result_t r;
        int rc = res_decode(call_info(), &r);
        snprintf(detail, sizeof detail, "rc=%d err=%s on=%ld w=%ld h=%ld",
                 rc, r.error, r.on, r.w, r.h);
        check(rc == 0 && !r.has_error && r.has_sensor
              && strcmp(r.sensor, "gc2145") == 0
              && r.has_on && r.on == 0
              && r.has_w && r.w == 0 && r.has_h && r.h == 0
              && !r.has_framesize,
              "T03 info() before init: {sensor, on=0, w=0, h=0, no framesize}",
              detail);
        check(s_ev_n == 0, "T03b info() before init touches no host", "");
    }

    /* T04: unknown framesize refused before any hardware touch */
    {
        result_t r;
        int rc = res_decode(call_init_fs("SXGA"), &r);
        snprintf(detail, sizeof detail, "rc=%d err=%s events=%d",
                 rc, r.error, s_ev_n);
        check(rc == 0 && r.has_error && strcmp(r.error, "unknown framesize") == 0,
              "T04 unknown framesize -> honest error", detail);
        check(s_ev_n == 0, "T04b refusal touches no host (no xclk/i2c/cam)", "");
    }

    /* T05: tolerant default — no args = QVGA */
    {
        result_t r;
        int rc = res_decode(call_init_noargs(), &r);
        snprintf(detail, sizeof detail, "rc=%d err=%s w=%ld h=%ld",
                 rc, r.error, r.w, r.h);
        check(rc == 0 && !r.has_error && r.has_ok && r.ok == 1
              && r.has_w && r.w == 320 && r.has_h && r.h == 240,
              "T05 init() no args defaults to QVGA 320x240", detail);
        /* off() drains to a clean state for the anatomy run */
        res_decode(call_off(), &r);
        check(!r.has_error && r.has_on && r.on == 0,
              "T05b off() after init", "");
        host_reset();
    }

    /* T06..T11: full anatomy of one QVGA init from the off state */
    {
        result_t r;
        int rc = res_decode(call_init_fs("QVGA"), &r);
        snprintf(detail, sizeof detail, "rc=%d err=%s ok=%ld sensor=%s w=%ld h=%ld",
                 rc, r.error, r.ok, r.sensor, r.w, r.h);
        check(rc == 0 && !r.has_error && r.has_ok && r.ok == 1
              && r.has_sensor && strcmp(r.sensor, "gc2145") == 0
              && r.has_w && r.w == 320 && r.has_h && r.h == 240,
              "T06 init{QVGA} -> {ok=1, sensor=gc2145, w=320, h=240}", detail);
        check(r.has_ms && r.ms >= 2200,
              "T06b init reports ms >= warmup+delays (2200)", detail);

        /* T07: xclk -> warmup 2000ms (100ms slices) -> i2c_config(0,4,5,100k)
         * -> probe, in exactly that order */
        const ev_t *xclk = ev_find(EV_XCLK, 0);
        const ev_t *cfg = ev_find(EV_ICONFIG, 0);
        snprintf(detail, sizeof detail, "xclk=%d cfg=%d n=%d",
                 ev_idx(xclk), ev_idx(cfg), s_ev_n);
        check(xclk != NULL && xclk->a == 20000000,
              "T07 cam_xclk(20000000) called", detail);
        check(cfg != NULL && cfg->a == 0 && cfg->b == 4 && cfg->c == 5
              && cfg->d == 100000,
              "T07b i2c_config(slot0, sda=4, scl=5, 100000)", detail);
        check(xclk && cfg && ev_idx(xclk) < ev_idx(cfg)
              && delay_sum_between(ev_idx(xclk), ev_idx(cfg)) == 2000,
              "T07c warmup: exactly 2000ms between xclk and i2c_config", detail);
        {
            int slices = 0;
            for (int i = ev_idx(xclk) + 1; i < ev_idx(cfg); i++)
                if (s_ev[i].kind == EV_DELAY) {
                    slices++;
                    if (s_ev[i].ms != 100) slices = -1000;
                }
            snprintf(detail, sizeof detail, "slices=%d", slices);
            check(slices == 20, "T07d warmup sliced as 20 x delay_ms(100)", detail);
        }

        /* T08: PID probe = two writereads (0xF0 then 0xF1) BEFORE any write */
        {
            const ev_t *r0 = ev_find(EV_IWREAD, 0);
            const ev_t *r1 = ev_find(EV_IWREAD, ev_idx(r0) + 1);
            const ev_t *w0 = ev_find(EV_IWRITE, 0);
            snprintf(detail, sizeof detail, "r0=%d r1=%d w0=%d",
                     ev_idx(r0), ev_idx(r1), ev_idx(w0));
            check(r0 && r1 && r0->b == 0x3C && r0->wlen == 1
                  && r0->wbuf[0] == 0xF0 && r0->wlen == 1
                  && r1->b == 0x3C && r1->wbuf[0] == 0xF1
                  && ev_idx(r1) == ev_idx(r0) + 1,
                  "T08 probe: writeread 0x3C {0xF0} then {0xF1}", detail);
            check(w0 && ev_idx(r1) < ev_idx(w0),
                  "T08b probe precedes every i2c_write", detail);
        }

        /* T09: soft reset first write + its 100ms wait before the table */
        {
            const ev_t *w0 = ev_find(EV_IWRITE, 0);
            int widx[700];
            int nw = w_list(widx, 700);
            snprintf(detail, sizeof detail, "w0=[%02x,%02x] nw=%d",
                     w0 ? w0->wbuf[0] : 0, w0 ? w0->wbuf[1] : 0, nw);
            check(w0 && w0->wbuf[0] == 0xFE && w0->wbuf[1] == 0xE0
                  && w0->wlen == 2 && w0->b == 0x3C,
                  "T09 soft reset: FIRST i2c_write is {0xFE, 0xE0} @0x3C", detail);
            check(nw == 1 + (int)EXP_TABLE_N + (int)GC2145_FS_REGS_N + 2,
                  "T09b write count: 1 reset + 582 table + 23 fs + 2 fmt", detail);
            check(delay_sum_between(widx[0], widx[1]) == 100,
                  "T09c ~100ms between soft reset and the init table", detail);
        }

        /* T10: the full init table, pair-by-pair, in table order; the
         * transcription itself guarded by the vendored-source constants */
        {
            int widx[700];
            int nw = w_list(widx, 700);
            int ok = nw > (int)EXP_TABLE_N;
            for (uint32_t i = 0; ok && i < EXP_TABLE_N; i++) {
                const ev_t *e = &s_ev[widx[1 + (int)i]];
                if (e->b != 0x3C || e->wlen != 2
                    || e->wbuf[0] != gc2145_init_regs[i][0]
                    || e->wbuf[1] != gc2145_init_regs[i][1]) ok = 0;
            }
            snprintf(detail, sizeof detail, "nw=%d table_ok=%d", nw, ok);
            check(ok, "T10 init table delivered pair-by-pair in order", detail);
            snprintf(detail, sizeof detail, "fnv=%08x want=%08x",
                     table_fnv(), EXP_TABLE_FNV);
            check(GC2145_INIT_REGS_N == EXP_TABLE_N
                  && table_fnv() == EXP_TABLE_FNV,
                  "T10b transcription guard: 582 pairs, FNV-1a 0x89b1a596",
                  detail);
        }

        /* T11: table tail — 100ms post-table wait, QVGA geometry deltas,
         * RGB565 format, cam_start(320,240,0) LAST */
        {
            int widx[700];
            (void)w_list(widx, 700);
            int t_end = widx[1 + (int)EXP_TABLE_N - 1];   /* last table write */
            int fs0 = widx[1 + (int)EXP_TABLE_N];         /* first fs write  */
            int ok = delay_sum_between(t_end, fs0) == 100;
            static const uint8_t qvga_tail[23][2] = {
                {0xfe, 0x00}, {0x90, 0x01},
                {0x09, 0x01}, {0x0a, 0x68}, {0x0b, 0x01}, {0x0c, 0xe0},
                {0x0d, 0x01}, {0x0e, 0xe8}, {0x0f, 0x02}, {0x10, 0x90},
                {0x99, 0x22}, {0x9b, 0x00}, {0x9c, 0x00}, {0x9d, 0x00},
                {0x9e, 0x00}, {0x9f, 0x00}, {0xa0, 0x00}, {0xa1, 0x00},
                {0xa2, 0x00}, {0x95, 0x00}, {0x96, 0xf0},
                {0x97, 0x01}, {0x98, 0x40},
            };
            for (int i = 0; ok && i < 23; i++) {
                const ev_t *e = &s_ev[widx[1 + (int)EXP_TABLE_N + i]];
                if (e->wbuf[0] != qvga_tail[i][0]
                    || e->wbuf[1] != qvga_tail[i][1]) ok = 0;
            }
            snprintf(detail, sizeof detail, "post100=%u tail_ok=%d",
                     delay_sum_between(t_end, fs0), ok);
            check(ok, "T11 100ms post-table, then the QVGA subsample deltas",
                  detail);
            {
                const ev_t *fmt1 = &s_ev[widx[1 + (int)EXP_TABLE_N + 23]];
                const ev_t *fmt2 = &s_ev[widx[1 + (int)EXP_TABLE_N + 24]];
                snprintf(detail, sizeof detail, "fmt=[%02x,%02x][%02x,%02x]",
                        fmt1->wbuf[0], fmt1->wbuf[1],
                        fmt2->wbuf[0], fmt2->wbuf[1]);
                check(fmt1->wbuf[0] == 0xfe && fmt1->wbuf[1] == 0x00
                      && fmt2->wbuf[0] == 0x84 && fmt2->wbuf[1] == 0x06,
                      "T11b RGB565 output format regs {0xfe,0x00}{0x84,0x06}",
                      detail);
            }
            const ev_t *st = ev_find(EV_CSTART, 0);
            snprintf(detail, sizeof detail, "start=%dx%d pf=%u last=%d n=%d",
                     st ? (int)st->a : -1, st ? (int)st->b : -1,
                     st ? st->c : 999u, st ? ev_idx(st) : -1, s_ev_n);
            check(st && st->a == 320 && st->b == 240 && st->c == 0
                  && ev_idx(st) == s_ev_n - 1,
                  "T11c cam_start(320,240,RGB565) is the final host call",
                  detail);
        }

        /* leave ON (QVGA) for the switch scenario */
    }

    /* T12: PID mismatch — honest error carrying the read PID, no side
     * effects past the probe */
    {
        call_off();
        host_reset();
        s_pid = 0x9999;
        result_t r;
        int rc = res_decode(call_init_fs("QVGA"), &r);
        snprintf(detail, sizeof detail,
                 "rc=%d err=%s got=%04lx want=%04lx w=%d s=%d",
                 rc, r.error, r.got, r.want,
                 (int)(ev_find(EV_IWRITE, 0) != NULL),
                 (int)(ev_find(EV_CSTART, 0) != NULL));
        check(rc == 0 && r.has_error && strstr(r.error, "0x9999") != NULL
              && r.has_got && r.got == 0x9999
              && r.has_want && r.want == 0x2145,
              "T12 PID mismatch: error carries the read PID", detail);
        check(ev_find(EV_IWRITE, 0) == NULL && ev_find(EV_CSTART, 0) == NULL,
              "T12b mismatch aborts before soft reset / table / cam_start",
              detail);
        s_pid = 0x2145;
    }

    /* T13: an SCCB failure mid-table traps out with the host message
     * VERBATIM (the module must not swallow it), then the module
     * recovers on the next init */
    {
        host_reset();
        s_fail_at = 300;               /* NACK at table write #299-ish */
        int trapped = 0;
        if (setjmp(s_jmp) == 0) {
            s_trap_armed = 1;
            call_init_fs("QVGA");
            s_trap_armed = 0;
        } else {
            s_trap_armed = 0;
            trapped = 1;
        }
        snprintf(detail, sizeof detail, "trapped=%d msg=%s",
                 trapped, s_trap_msg);
        check(trapped && strcmp(s_trap_msg,
                                "xuansi.i2c_write: nack (programmed)") == 0,
              "T13 SCCB failure propagates verbatim (trap)", detail);
        {
            int widx[700];
            int nw = w_list(widx, 700);
            snprintf(detail, sizeof detail, "nw=%d", nw);
            check(nw == 300 - 1, "T13b trap lands at the programmed write", detail);
        }
        host_reset();                   /* module state must recover */
        result_t r;
        int rc = res_decode(call_init_fs("QVGA"), &r);
        snprintf(detail, sizeof detail, "rc=%d err=%s", rc, r.error);
        check(rc == 0 && !r.has_error && r.has_ok && r.ok == 1
              && r.w == 320,
              "T13c module recovers: next init is clean", detail);
    }

    /* T14: framesize switch QVGA -> VGA: cam_stop, settle >= 1000ms
     * BEFORE the next host touch, full re-init (probe + soft reset +
     * table), VGA deltas, cam_start(640,480,0) */
    {
        host_reset();
        result_t r;
        int rc = res_decode(call_init_fs("VGA"), &r);
        snprintf(detail, sizeof detail, "rc=%d err=%s w=%ld h=%ld",
                 rc, r.error, r.w, r.h);
        check(rc == 0 && !r.has_error && r.has_ok && r.ok == 1
              && r.has_w && r.w == 640 && r.has_h && r.h == 480,
              "T14 init{VGA} after QVGA -> {ok=1, w=640, h=480}", detail);
        {
            const ev_t *stop = ev_find(EV_CSTOP, 0);
            int next = stop ? ev_idx(stop) + 1 : -1;
            while (next >= 0 && next < s_ev_n && s_ev[next].kind == EV_DELAY)
                next++;
            uint32_t settle = delay_sum_between(ev_idx(stop), next);
            snprintf(detail, sizeof detail, "stop=%d next_kind=%c settle=%u",
                     ev_idx(stop), next < s_ev_n ? s_ev[next].kind : '?',
                     settle);
            check(stop && ev_idx(stop) == 0
                  && settle >= 1000,
                  "T14b cam_stop FIRST, then >= 1000ms settle before re-init",
                  detail);
            snprintf(detail, sizeof detail, "next=%c",
                     next < s_ev_n ? s_ev[next].kind : '?');
            check(next < s_ev_n && s_ev[next].kind == EV_XCLK,
                  "T14c re-init restarts at cam_xclk (full sequence)", detail);
        }
        {
            /* second soft reset + full table + VGA tail */
            int widx[700];
            int nw = w_list(widx, 700);
            int ok = nw == 1 + (int)EXP_TABLE_N + (int)GC2145_FS_REGS_N + 2;
            const ev_t *w0 = nw ? &s_ev[widx[0]] : NULL;
            snprintf(detail, sizeof detail, "nw=%d w0=[%02x,%02x]",
                     nw, w0 ? w0->wbuf[0] : 0, w0 ? w0->wbuf[1] : 0);
            check(ok && w0 && w0->wbuf[0] == 0xFE && w0->wbuf[1] == 0xE0,
                  "T14d re-init: soft reset first, table fully redelivered",
                  detail);
            static const uint8_t vga_tail[23][2] = {
                {0xfe, 0x00}, {0x90, 0x01},
                {0x09, 0x00}, {0x0a, 0x78}, {0x0b, 0x00}, {0x0c, 0xa0},
                {0x0d, 0x03}, {0x0e, 0xc8}, {0x0f, 0x05}, {0x10, 0x10},
                {0x99, 0x22}, {0x9b, 0x00}, {0x9c, 0x00}, {0x9d, 0x00},
                {0x9e, 0x00}, {0x9f, 0x00}, {0xa0, 0x00}, {0xa1, 0x00},
                {0xa2, 0x00}, {0x95, 0x01}, {0x96, 0xe0},
                {0x97, 0x02}, {0x98, 0x80},
            };
            int tail_ok = ok;
            for (int i = 0; tail_ok && i < 23; i++) {
                const ev_t *e = &s_ev[widx[1 + (int)EXP_TABLE_N + i]];
                if (e->wbuf[0] != vga_tail[i][0]
                    || e->wbuf[1] != vga_tail[i][1]) tail_ok = 0;
            }
            snprintf(detail, sizeof detail, "tail_ok=%d", tail_ok);
            check(tail_ok, "T14e VGA subsample deltas delivered in order",
                  detail);
            const ev_t *st = ev_find(EV_CSTART, 0);
            snprintf(detail, sizeof detail, "start=%dx%d",
                     st ? (int)st->a : -1, st ? (int)st->b : -1);
            check(st && st->a == 640 && st->b == 480 && st->c == 0,
                  "T14f cam_start(640,480,RGB565)", detail);
        }
    }

    /* T15: info reflects the live state; off() is idempotent */
    {
        result_t r;
        int rc = res_decode(call_info(), &r);
        snprintf(detail, sizeof detail,
                 "rc=%d fs=%s on=%ld w=%ld h=%ld pf=%ld",
                 rc, r.framesize, r.on, r.w, r.h, r.pixfmt);
        check(rc == 0 && r.has_framesize && strcmp(r.framesize, "VGA") == 0
              && r.has_on && r.on == 1
              && r.has_w && r.w == 640 && r.has_h && r.h == 480
              && r.has_pixfmt && r.pixfmt == 0,
              "T15 info() while on: {VGA, on=1, 640x480, RGB565}", detail);
        rc = res_decode(call_off(), &r);
        snprintf(detail, sizeof detail, "rc=%d err=%s", rc, r.error);
        check(rc == 0 && !r.has_error, "T15b off() -> ok", detail);
        rc = res_decode(call_info(), &r);
        snprintf(detail, sizeof detail, "on=%ld w=%ld h=%ld fs=%s",
                 r.on, r.w, r.h, r.framesize);
        check(rc == 0 && r.has_on && r.on == 0 && r.w == 0 && r.h == 0
              && r.has_framesize && strcmp(r.framesize, "VGA") == 0,
              "T15c info() after off: on=0, geometry zeroed, framesize kept",
              detail);
        host_reset();
        rc = res_decode(call_off(), &r);
        const ev_t *stop = ev_find(EV_CSTOP, 0);
        snprintf(detail, sizeof detail, "rc=%d err=%s stop=%d",
                 rc, r.error, ev_idx(stop));
        check(rc == 0 && !r.has_error && stop && ev_idx(stop) == 0
              && s_ev_n == 1,
              "T15d off() idempotent: one cam_stop, nothing else", detail);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
