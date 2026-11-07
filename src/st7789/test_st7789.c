/*
 * test_st7789.c — native unit test for the st7789 driver module (stub
 * host + panel model; zero hardware coupling).
 *
 * Build & run (host toolchain, no wasm needed):
 *   cc -DXUANSI_NATIVE_TEST -Wall -Wextra -Werror -I src -I src/st7789 \
 *      src/st7789/test_st7789.c -o build/test_st7789 \
 *      && ./build/test_st7789
 *
 * Same seam as test_hello.c / test_ledwatch.c: this file includes
 * st7789.c directly (one translation unit), provides the `xuansi`
 * host namespace as stubs, and drives every export through the u32-
 * offset msgpack marshalling the wasm host uses.
 *
 * The stub host ALSO models the panel: a 240x320 gate-source RAM with a
 * CASET/RASET/RAMWR interpreter fed by the DC pin level, so window
 * geometries (border strips, quadrants, the full frame) are asserted on
 * the pixels the glass would actually show. glass_off is the model's
 * panel property (which 240 of the 320 gates are bonded) — the module's
 * cfg x_offset is the module's BELIEF; the border-probe semantics
 * (flush at the edge = right candidate, inset 80 rows = wrong one) are
 * asserted for both matching and mismatching candidates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>

#include "st7789.c"    /* unity build: the module under test */

/* fix1 seam guard (same as test_ledwatch.c) */
#ifndef ARGS_SLOT_SZ
#define ARGS_SLOT_SZ 128
#endif

/* ---- stub host state ---- */

static uint32_t fake_ms;              /* monotonic fake clock */
static int      slot_held;            /* spi slot conflict simulation */
static uint32_t max_xfer_len;         /* engine-cap conformance */

/* event log (reset per scenario; T17 clears per call) */
typedef struct {
    char     type;                    /* G=pio D=elay C=fg X=fer */
    uint32_t a, b, c, d, e, f;        /* per-type payloads */
    uint32_t len;
    uint8_t  bytes[8];                /* first bytes of an xfer */
} ev_t;
#define EV_MAX 24000
static ev_t evs[EV_MAX];
static int ev_n;
static void ev_reset(void) { ev_n = 0; }

static void ev_push(char t, uint32_t a, uint32_t b, uint32_t c,
                    uint32_t d, uint32_t e, uint32_t f,
                    uint32_t len, const uint8_t *bp)
{
    if (ev_n >= EV_MAX) {
        fprintf(stderr, "FATAL: event overflow\n");
        exit(2);
    }
    ev_t *v = &evs[ev_n++];
    v->type = t; v->a = a; v->b = b; v->c = c; v->d = d; v->e = e; v->f = f;
    v->len = len;
    for (uint32_t i = 0; i < 8 && bp && i < len; i++) v->bytes[i] = bp[i];
}

/* ---- panel model (ST7789V RAM: 240 sources x 320 gates) ---- */

static uint16_t ram[320][240];        /* [gate][source] */
static uint32_t glass_off;            /* panel property: bonded gate start */
static uint32_t p_x0, p_x1, p_y0, p_y1;   /* current window (abs gates) */
static uint32_t cur_x, cur_y;         /* RAMWR raster cursor */
static int      in_ramwr, pix_half;
static uint8_t  pending_cmd, pix_acc;
static uint8_t  cmd_data[8];          /* CASET/RASET parameter buffer */
static uint32_t cmd_data_n;
static uint32_t cur_dc;               /* DC pin level (pin 21) */

static void panel_reset(uint32_t glass)
{
    glass_off = glass;
    memset(ram, 0, sizeof ram);
    p_x0 = p_x1 = p_y0 = p_y1 = 0;
    cur_x = cur_y = 0;
    in_ramwr = pix_half = 0;
    pending_cmd = pix_acc = 0;
    cmd_data_n = 0;
    cur_dc = 0;
}

/* visible glass pixel (x 0..239, y 0..239) -> gate y+glass_off */
static uint16_t vis(int x, int y)
{
    return ram[y + (int)glass_off][x];
}

static void win_apply(void)
{
    if (cmd_data_n != 4) return;
    uint32_t v0 = ((uint32_t)cmd_data[0] << 8) | cmd_data[1];
    uint32_t v1 = ((uint32_t)cmd_data[2] << 8) | cmd_data[3];
    if (pending_cmd == ST_CMD_CASET) { p_x0 = v0; p_x1 = v1; }
    else                            { p_y0 = v0; p_y1 = v1; }
    cmd_data_n = 0;
}

static void feed(uint8_t b)
{
    if (in_ramwr) {
        if (!pix_half) { pix_acc = b; pix_half = 1; return; }
        if (cur_y < 320 && cur_x < 240) ram[cur_y][cur_x] =
            (uint16_t)(((uint16_t)pix_acc << 8) | b);
        pix_half = 0;
        if (++cur_x > p_x1) { cur_x = p_x0;            /* window wrap */
            if ((int)++cur_y > (int)p_y1) cur_y = p_y0; }
        return;
    }
    if (pending_cmd == ST_CMD_CASET || pending_cmd == ST_CMD_RASET) {
        if (cmd_data_n < 8) cmd_data[cmd_data_n++] = b;
        if (cmd_data_n == 4) win_apply();
    }
    /* COLMOD/MADCTL parameters: recorded in the event bytes only */
}

/* ---- trap emulation (host refusal channel) ---- */

static jmp_buf s_jmp;
static int     s_trap_armed;
static char    s_trap_msg[128];
static void stub_raise(const char *msg)
{
    if (!s_trap_armed) {
        fprintf(stderr, "FATAL: stub host raised outside an armed trap: %s\n",
                msg);
        exit(2);
    }
    snprintf(s_trap_msg, sizeof s_trap_msg, "%s", msg);
    longjmp(s_jmp, 1);
}

/* ---- the `xuansi` namespace stubs (only what st7789 imports) ---- */

void xuansi_millis(uint32_t *out)
{
    fake_ms += 7;
    *out = fake_ms;
}

void xuansi_delay_ms(uint32_t ms)
{
    ev_push('D', ms, 0, 0, 0, 0, 0, 0, NULL);
    fake_ms += ms;
}

void xuansi_gpio_set(uint32_t pin, uint32_t level)
{
    ev_push('G', pin, level, 0, 0, 0, 0, 0, NULL);
    if (pin == PIN_DC) cur_dc = level;
}

void xuansi_gpio_get(uint32_t pin, uint32_t *out)
{
    (void)pin;
    *out = 0;
}

void xuansi_spi_config(uint32_t *out, uint32_t slot, uint32_t sclk_pin,
                       uint32_t sclk_hz, int32_t mosi, int32_t miso,
                       uint32_t mode, uint32_t no_cs)
{
    ev_push('C', slot, sclk_pin, sclk_hz, (uint32_t)mosi,
            (uint32_t)miso, (mode << 1) | no_cs, 0, NULL);
    if (slot_held)
        stub_raise("xuansi.spi_config: slot 0 setup refused: "
                   "slot held by another owner (rc=-5)");
    *out = 0;
}

void xuansi_spi_xfer(uint32_t *out, uint32_t slot, uint32_t wbuf_off,
                     uint32_t wlen, uint32_t rbuf_off, uint32_t rcap)
{
    const uint8_t *p = xuansi_lin_base + wbuf_off;
    (void)slot; (void)rbuf_off; (void)rcap;
    if (wlen > max_xfer_len) max_xfer_len = wlen;
    ev_push('X', cur_dc, wlen, 0, 0, 0, 0, wlen <= 8 ? wlen : 8, p);
    if (cur_dc) {
        for (uint32_t i = 0; i < wlen; i++) feed(p[i]);
    } else if (wlen >= 1) {
        pending_cmd = p[0];
        cmd_data_n = 0;
        if (pending_cmd == ST_CMD_RAMWR) {
            in_ramwr = 1;
            cur_x = p_x0; cur_y = p_y0; pix_half = 0;
        } else {
            in_ramwr = 0;
        }
    }
    *out = wlen;
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

static const uint8_t *s_res_p;
static uint32_t s_res_len, s_res_pos;

/* decoded result: ints + strs by key */
#define RES_KEYS 12
static char   res_keys[RES_KEYS][16];
static int64_t res_ints[RES_KEYS];
static char   res_strs[RES_KEYS][40];
static int    res_is_str[RES_KEYS];
static int    res_n;

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

static int rd_val(int64_t *iv, char *sv)
{
    uint8_t t;
    if (rd_u8(&t) != 0) return -1;
    if ((t & 0xe0) == 0xa0) {                 /* fixstr value */
        uint32_t n = t & 0x1f;
        if (s_res_pos + n > s_res_len || n >= 40) return -1;
        for (uint32_t i = 0; i < n; i++) sv[i] = (char)s_res_p[s_res_pos++];
        sv[n] = 0;
        return 2;
    }
    s_res_pos--;                              /* push back type byte */
    return rd_i64(iv) == 0 ? 1 : -1;
}

static int res_decode(uint64_t packed)
{
    res_n = 0;
    s_res_p = arena + (uint32_t)(packed >> 32);
    s_res_len = (uint32_t)packed;
    s_res_pos = 0;
    uint8_t t;
    if (rd_u8(&t) != 0 || (t & 0xf0) != 0x80) return -1;   /* fixmap */
    uint32_t n = t & 0x0f;
    for (uint32_t i = 0; i < n; i++) {
        if (rd_u8(&t) != 0 || (t & 0xe0) != 0xa0) return -1;
        uint32_t klen = t & 0x1f;
        if (klen >= 16 || s_res_pos + klen > s_res_len) return -1;
        char key[16];
        for (uint32_t k = 0; k < klen; k++) key[k] = (char)s_res_p[s_res_pos++];
        key[klen] = 0;
        int64_t iv = 0;
        char sv[40];
        int r = rd_val(&iv, sv);
        if (r < 0 || res_n >= RES_KEYS) return -1;
        snprintf(res_keys[res_n], 16, "%s", key);
        res_ints[res_n] = iv;
        snprintf(res_strs[res_n], 40, "%s", sv);
        res_is_str[res_n] = (r == 2);
        res_n++;
    }
    return 0;
}

static int kidx(const char *key)              /* -1 if absent */
{
    for (int i = 0; i < res_n; i++)
        if (strcmp(res_keys[i], key) == 0) return i;
    return -1;
}

static long ri(const char *key, long dflt)
{
    int i = kidx(key);
    return (i >= 0 && !res_is_str[i]) ? (int)res_ints[i] : dflt;
}

static const char *rstr(const char *key)
{
    int i = kidx(key);
    return (i >= 0 && res_is_str[i]) ? res_strs[i] : NULL;
}

/* ---- args builders (the wasm host's alloc + write step) ---- */

static uint32_t host_write_args(const uint8_t *bytes, uint32_t n)
{
    uint32_t off = xuansi_alloc(n);
    if (off == 0) { fprintf(stderr, "FATAL: alloc refused\n"); exit(2); }
    memcpy(arena + off, bytes, n);
    return off;
}

static void put_int(uint8_t *b, uint32_t *pos, int64_t v)
{
    if (v >= 0 && v < 128) { b[(*pos)++] = (uint8_t)v; }
    else if (v >= 0 && v < 0x10000) {
        b[(*pos)++] = 0xcd;
        b[(*pos)++] = (uint8_t)(v >> 8); b[(*pos)++] = (uint8_t)v;
    } else if (v >= 0) {
        b[(*pos)++] = 0xce;
        b[(*pos)++] = (uint8_t)(v >> 24); b[(*pos)++] = (uint8_t)(v >> 16);
        b[(*pos)++] = (uint8_t)(v >> 8);  b[(*pos)++] = (uint8_t)v;
    } else {
        b[(*pos)++] = 0xd0;                      /* int8 (negatives) */
        b[(*pos)++] = (uint8_t)v;
    }
}

/* cfg map builder — "x_offset=i:80,mode=i:3,invon=b:1" (ledwatch style;
 * ints encode at their natural msgpack width) */
static uint32_t build_cfg(uint8_t *buf, const char *kvs)
{
    uint32_t pos = 0, nkv = 0;
    const char *p = kvs;
    while (*p) { nkv++; while (*p && *p != ',') p++; if (*p) p++; }
    buf[pos++] = 0x91;                          /* fixarray(1) */
    buf[pos++] = (uint8_t)(0x80 | nkv);         /* fixmap(nkv) */
    p = kvs;
    for (uint32_t i = 0; i < nkv; i++) {
        const char *eq = strchr(p, '=');
        const char *colon = strchr(p, ':');
        const char *comma = strchr(p, ',');
        size_t klen = (size_t)(eq - p);
        const char *vend = comma ? comma : (p + strlen(p));
        size_t vlen = (size_t)(vend - colon - 1);
        char val[32];
        buf[pos++] = (uint8_t)(0xa0 | klen);
        memcpy(buf + pos, p, klen); pos += (uint32_t)klen;
        memcpy(val, colon + 1, vlen); val[vlen] = 0;
        if (*(eq + 1) == 'i') put_int(buf, &pos, strtol(val, NULL, 10));
        else buf[pos++] = (uint8_t)(strcmp(val, "1") == 0 ? 0xc3 : 0xc2);
        p = comma ? comma + 1 : p + strlen(p);
    }
    return pos;
}

static uint64_t call_init(const char *kvs)
{
    uint8_t abuf[160];
    uint32_t n, off;
    if (kvs && kvs[0]) { n = build_cfg(abuf, kvs); off = host_write_args(abuf, n); }
    else { abuf[0] = 0x90; n = 1; off = host_write_args(abuf, n); }
    return st_init(off, n);
}

static uint64_t call_int_fn(uint64_t (*fn)(uint32_t, uint32_t), int has,
                            int64_t v)
{
    uint8_t abuf[16];
    uint32_t n = 0, pos = 0;
    abuf[pos++] = 0x91;
    if (has) put_int(abuf, &pos, v);
    else { abuf[pos - 1] = 0x90; }              /* fixarray(0): no args */
    n = pos;
    uint32_t off = host_write_args(abuf, n);
    return fn(off, n);
}

static uint64_t call_pattern(const char *kind)
{
    uint8_t abuf[24];
    uint32_t pos = 0;
    abuf[pos++] = 0x91;
    abuf[pos++] = (uint8_t)(0xa0 | strlen(kind));
    memcpy(abuf + pos, kind, strlen(kind)); pos += (uint32_t)strlen(kind);
    uint32_t off = host_write_args(abuf, pos);
    return st_pattern(off, pos);
}

static uint64_t call_noargs(uint64_t (*fn)(uint32_t, uint32_t))
{
    uint8_t abuf[2] = { 0x90, 0 };
    uint32_t off = host_write_args(abuf, 1);
    return fn(off, 1);
}

#define EXPECT_TRAP(expr, msgfrag)                                     \
    do {                                                               \
        s_trap_armed = 1; s_trap_msg[0] = 0;                           \
        if (setjmp(s_jmp) == 0) {                                      \
            (void)(expr);                                              \
            s_trap_armed = 0;                                          \
            check(0, msgfrag, "no trap raised");                       \
        } else {                                                       \
            s_trap_armed = 0;                                          \
            check(strstr(s_trap_msg, "xuansi.") == s_trap_msg,         \
                  msgfrag, s_trap_msg);                                \
        }                                                              \
    } while (0)

/* fresh board-ish state before each scenario */
static void reset_state(uint32_t glass)
{
    fake_ms = 0;
    slot_held = 0;
    max_xfer_len = 0;
    ev_reset();
    panel_reset(glass);
    memset(arena, 0, sizeof arena);
    st_inited = 0;                              /* module statics */
    st_gate_off = DEF_GATE_OFF;
    st_mode = DEF_MODE;
    st_invon = 1;
    heap_off = 8;
    xuansi_native_init(arena);
}

/* event helpers: find the i'th xfer with DC=0 (command byte) */
static int ev_cmd(int i, uint8_t *cmd)
{
    int seen = 0;
    for (int k = 0; k < ev_n; k++) {
        if (evs[k].type == 'X' && evs[k].a == 0) {
            if (seen == i) { *cmd = evs[k].bytes[0]; return k; }
            seen++;
        }
    }
    return -1;
}

/* data xfer right after command event idx (same transaction's payload) */
static int ev_cmd_data(int idx, uint8_t out[4])
{
    for (int k = idx + 1; k < ev_n; k++) {
        if (evs[k].type != 'X') continue;
        if (evs[k].a == 1 && evs[k].len <= 8) {   /* DC-high, short */
            for (uint32_t i = 0; i < evs[k].len && i < 4; i++)
                out[i] = evs[k].bytes[i];
            return (int)evs[k].len;
        }
        break;
    }
    return -1;
}

int main(void)
{
    char detail[256];
    uint8_t cmd, d4[4];

    /* ================= T01 abi + allocator ================= */
    reset_state(0);
    check(xuansi_abi() == 1, "T01 xuansi_abi()==1", "");
    check(xuansi_alloc(0) == 0, "T01b alloc(0) refused", "");
    {
        uint32_t o1 = xuansi_alloc(64);
        uint32_t o2 = xuansi_alloc(64);
        check(o1 != 0 && o1 == o2,
              "T01c small allocs reuse the args slot (§3.1)", NULL);
        uint32_t b1 = xuansi_alloc(2048);
        uint32_t b2 = xuansi_alloc(2048);
        check(b1 != 0 && b2 != 0 && b1 != b2,
              "T01d large allocs bump (one-shot path)", "");
    }

    /* ================= T02 not-initialized refusals ================= */
    {
        uint64_t r = call_int_fn(st_clear, 1, 0xF800);
        int ok = res_decode(r) == 0 && rstr("error")
              && strcmp(rstr("error"), "not initialized") == 0;
        check(ok, "T02a clear before init -> {error='not initialized'}", "");
        r = call_int_fn(st_blit, 0, 0);
        ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "not initialized") == 0;
        check(ok, "T02b blit before init refused", "");
        r = call_pattern("border");
        ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "not initialized") == 0;
        check(ok, "T02c pattern before init refused", "");
        r = call_noargs(st_deinit);
        ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "not initialized") == 0;
        check(ok, "T02d deinit before init refused", "");
    }

    /* ================= T03 init default byte stream (glass=0) =========
     * spi_config(slot0, sclk=41, 40MHz, mosi=42, miso=-1, mode=3, no_cs=1)
     * RES low 10ms -> high -> 120ms -> SLPOUT -> 120ms -> COLMOD 0x55 ->
     * MADCTL 0x00 -> INVON -> CASET 0..239 -> RASET 0..239 -> DISPON   */
    {
        uint64_t r = call_init("");
        int dec = res_decode(r);
        int ok = dec == 0 && ri("ok", 0) == 1 && ri("x_offset", -1) == 0
              && ri("mode", -1) == 3 && ri("invon", -1) == 1
              && ri("ms", -1) >= 250;
        snprintf(detail, sizeof detail,
                 "dec=%d xo=%ld mode=%ld invon=%ld ms=%ld", dec,
                 ri("x_offset", -1), ri("mode", -1), ri("invon", -1),
                 ri("ms", -1));
        check(ok, "T03 init default result map", detail);

        /* first event = spi_config with the board pins */
        ok = ev_n >= 1 && evs[0].type == 'C' && evs[0].a == 0
          && evs[0].b == 41 && evs[0].c == 40000000
          && evs[0].d == 42 && (int32_t)evs[0].e == 14
          && evs[0].f == ((3u << 1) | 1u);
        snprintf(detail, sizeof detail,
                 "C(slot=%u sclk=%u hz=%u mosi=%u miso=%d f=%u)",
                 evs[0].a, evs[0].b, evs[0].c, evs[0].d,
                 (int32_t)evs[0].e, evs[0].f);
        check(ok, "T03b spi_config(0,41,40MHz,42,14,mode3,no_cs1) first",
              detail);

        /* reset pulse */
        int gi = -1;
        for (int k = 0; k < ev_n; k++)
            if (evs[k].type == 'G') { gi = k; break; }
        ok = gi == 1 && evs[gi].a == 47 && evs[gi].b == 0
          && evs[gi + 1].type == 'D' && evs[gi + 1].a == 10
          && evs[gi + 2].type == 'G' && evs[gi + 2].a == 47
          && evs[gi + 2].b == 1
          && evs[gi + 3].type == 'D' && evs[gi + 3].a == 120;
        check(ok, "T03c RES low 10ms -> high -> 120ms power-up wait", "");

        /* command stream */
        int ci = ev_cmd(0, &cmd);
        ok = ci > 0 && cmd == ST_CMD_SLPOUT
          && evs[ci + 1].type == 'D' && evs[ci + 1].a == 120;
        check(ok, "T03d SLPOUT + 120ms recovery", "");

        ev_cmd(1, &cmd);
        int dn = ev_cmd_data(ev_cmd(1, &cmd), d4);
        ok = cmd == ST_CMD_COLMOD && dn == 1 && d4[0] == 0x55;
        snprintf(detail, sizeof detail, "cmd=%02x dn=%d d0=%02x",
                 cmd, dn, d4[0]);
        check(ok, "T03e COLMOD 0x55 (16-bit)", detail);

        ev_cmd(2, &cmd);
        dn = ev_cmd_data(ev_cmd(2, &cmd), d4);
        ok = cmd == ST_CMD_MADCTL && dn == 1 && d4[0] == 0x00;
        check(ok, "T03f MADCTL 0x00 (portrait RGB)", "");

        ev_cmd(3, &cmd);
        ok = cmd == ST_CMD_INVON;
        check(ok, "T03g INVON (default invon=true)", "");

        int k4 = ev_cmd(4, &cmd);
        dn = ev_cmd_data(k4, d4);
        ok = cmd == ST_CMD_CASET && dn == 4
          && d4[0] == 0 && d4[1] == 0 && d4[2] == 0 && d4[3] == 0xEF;
        snprintf(detail, sizeof detail, "CASET %02x%02x %02x%02x",
                 d4[0], d4[1], d4[2], d4[3]);
        check(ok, "T03h CASET 0..239 (source axis, no offset)", detail);

        int k5 = ev_cmd(5, &cmd);
        dn = ev_cmd_data(k5, d4);
        ok = cmd == ST_CMD_RASET && dn == 4
          && d4[0] == 0 && d4[1] == 0 && d4[2] == 0 && d4[3] == 0xEF;
        snprintf(detail, sizeof detail, "RASET %02x%02x %02x%02x",
                 d4[0], d4[1], d4[2], d4[3]);
        check(ok, "T03i RASET 0..239 (gate offset 0)", detail);

        ev_cmd(6, &cmd);
        check(cmd == ST_CMD_DISPON, "T03j DISPON last", "");
    }

    /* ================= T04 init parameterized offsets/knobs ============
     * re-init on the SAME module instance (the HIL offset-probe flow);
     * x_offset=80 must parameterize RASET only (CASET stays 0..239) */
    {
        ev_reset();
        uint64_t r = call_init("x_offset=i:80");
        int dec = res_decode(r);
        int k4 = ev_cmd(4, &cmd);
        int dn = ev_cmd_data(k4, d4);
        int ok = dec == 0 && ri("ok", 0) == 1 && ri("x_offset", -1) == 80
          && cmd == ST_CMD_CASET && dn == 4
          && d4[0] == 0 && d4[1] == 0 && d4[2] == 0 && d4[3] == 0xEF;
        check(ok, "T04a x_offset=80: CASET unchanged (0..239)", "");

        int k5 = ev_cmd(5, &cmd);
        dn = ev_cmd_data(k5, d4);
        ok = cmd == ST_CMD_RASET && dn == 4
          && d4[0] == 0x00 && d4[1] == 0x50            /* 80 */
          && d4[2] == 0x01 && d4[3] == 0x3F;           /* 319 = 80+239 */
        snprintf(detail, sizeof detail, "RASET %02x%02x %02x%02x",
                 d4[0], d4[1], d4[2], d4[3]);
        check(ok, "T04b x_offset=80: RASET 80..319 (u16 BE parameterized)",
              detail);

        /* clamps + invon=false */
        ev_reset();
        r = call_init("x_offset=i:999,mode=i:9,invon=b:0");
        int inv_cmds = 0;
        for (int k = 0; k < ev_n; k++)
            if (evs[k].type == 'X' && evs[k].a == 0
                && evs[k].bytes[0] == ST_CMD_INVON) inv_cmds++;
        ok = res_decode(r) == 0 && ri("x_offset", -1) == 80
          && ri("mode", -1) == 3 && ri("invon", -1) == 0 && inv_cmds == 0;
        check(ok, "T04c x_offset>80 clamps to 80, mode>3 to 3, "
              "invon=false drops INVON", "");

        ev_reset();
        r = call_init("mode=i:0");
        ok = res_decode(r) == 0 && ri("mode", -1) == 0
          && evs[0].type == 'C' && (evs[0].f >> 1) == 0;
        check(ok, "T04d mode=0 reaches spi_config", "");

        /* unknown cfg keys ignored */
        r = call_init("bogus=i:7,mode=i:2");
        ok = res_decode(r) == 0 && ri("mode", -1) == 2;
        check(ok, "T04e unknown cfg key ignored", "");
        call_init("");                            /* back to defaults */
    }

    /* ================= T05 clear: full-frame fb path ================= */
    {
        reset_state(0);
        call_init("");
        uint64_t r = call_int_fn(st_clear, 1, 0xF800);
        int dec = res_decode(r);
        int ok = dec == 0 && ri("ok", 0) == 1
          && ri("bytes", -1) == 115200 && ri("chunks", -1) == 29
          && ri("total_ms", -1) == ri("fill_ms", -2)
                                   + ri("xfer_ms", -2)
          && ri("fill_ms", -1) >= 0 && ri("xfer_ms", -1) >= 0;
        snprintf(detail, sizeof detail,
                 "bytes=%ld chunks=%ld fill=%ld xfer=%ld total=%ld",
                 ri("bytes", -1), ri("chunks", -1), ri("fill_ms", -1),
                 ri("xfer_ms", -1), ri("total_ms", -1));
        check(ok, "T05 clear red: map + fill+xfer=total + 29 chunks", detail);

        int bad = 0;
        for (int y = 0; y < 240; y += 17)
            for (int x = 0; x < 240; x += 13)
                if (vis(x, y) != 0xF800) bad++;
        check(bad == 0, "T05b panel shows solid red (fb full frame)", "");
        check(max_xfer_len <= 4096, "T05c every xfer <= 4096B (engine cap)",
              NULL);

        /* window bytes of the clear stream: CASET/RASET full + RAMWR */
        int k4 = ev_cmd(4, &cmd);
        (void)k4;
        /* draw-op window (after the 7 init commands): cmds 7,8 = CASET,
         * RASET of the draw, 9 = RAMWR */
        int kw = ev_cmd(7, &cmd);
        int dn = ev_cmd_data(kw, d4);
        ok = cmd == ST_CMD_CASET && dn == 4
          && d4[1] == 0 && d4[3] == 0xEF;
        check(ok, "T05d draw window CASET full", "");
        kw = ev_cmd(8, &cmd);
        dn = ev_cmd_data(kw, d4);
        ok = cmd == ST_CMD_RASET && dn == 4 && d4[1] == 0 && d4[3] == 0xEF;
        check(ok, "T05e draw window RASET full", "");
        ev_cmd(9, &cmd);
        check(cmd == ST_CMD_RAMWR, "T05f RAMWR before the data stream", "");
    }

    /* ================= T06 pattern border (glass=0, off=0) ============= */
    {
        uint64_t r = call_pattern("border");
        int dec = res_decode(r);
        int ok = dec == 0 && ri("ok", 0) == 1 && rstr("kind")
          && strcmp(rstr("kind"), "border") == 0
          && ri("windows", -1) == 4
          && ri("bytes", -1) == (3840 + 3840 + 3584 + 3584);
        snprintf(detail, sizeof detail, "windows=%ld bytes=%ld",
                 ri("windows", -1), ri("bytes", -1));
        check(ok, "T06 border map: 4 windows, strip byte total", detail);

        /* geometry: 8px white frame at the visible edge; interior keeps
         * the clear() red (partial windows don't touch the interior) */
        int bad = 0;
        for (int x = 0; x < 240; x++) {
            if (vis(x, 0) != C_WHITE || vis(x, 7) != C_WHITE) bad++;
            if (vis(x, 232) != C_WHITE || vis(x, 239) != C_WHITE) bad++;
        }
        for (int y = 8; y < 232; y++) {
            if (vis(0, y) != C_WHITE || vis(7, y) != C_WHITE) bad++;
            if (vis(232, y) != C_WHITE || vis(239, y) != C_WHITE) bad++;
        }
        /* interior: strictly inside the strips stays the clear() red */
        for (int x = 8; x < 232; x += 7)
            if (vis(x, 100) != C_RED) bad++;
        for (int y = 8; y < 232; y += 7)
            if (vis(100, y) != C_RED) bad++;
        check(bad == 0, "T06b border flush at glass edge; interior "
              "untouched (offset-probe geometry)", "");
    }

    /* ================= T07 wrong-candidate inset (glass=80, off=0) =====
     * the probe's discriminating power: with the module believing 0 on a
     * glass bonded at 80, the border lands inset by 80 visible rows */
    {
        reset_state(80);
        call_init("");
        call_int_fn(st_clear, 1, 0);
        call_pattern("border");
        int ok = vis(120, 0) == C_BLACK             /* no top strip */
          && vis(120, 80) != C_WHITE && vis(120, 159) == C_WHITE
                                               /* bottom strip inset 80 */
          && vis(120, 160) == C_BLACK
          && vis(0, 100) == C_WHITE && vis(239, 100) == C_WHITE;
                                               /* left/right still at edge */
        check(ok, "T07 wrong candidate: border inset ~80 rows (top strip "
              "off-glass, bottom at visible row 159)", "");
    }

    /* ================= T08 pattern quad ================= */
    {
        reset_state(0);
        call_init("");
        uint64_t r = call_pattern("quad");
        int dec = res_decode(r);
        int ok = dec == 0 && ri("windows", -1) == 4
          && ri("bytes", -1) == 4 * 28800;
        check(ok, "T08 quad map: 4 quadrant windows", "");

        ok = vis(0, 0) == C_RED && vis(119, 119) == C_RED
          && vis(120, 0) == C_GREEN && vis(239, 119) == C_GREEN
          && vis(0, 120) == C_BLUE && vis(119, 239) == C_BLUE
          && vis(120, 120) == C_WHITE && vis(239, 239) == C_WHITE
          && vis(60, 60) == C_RED && vis(180, 60) == C_GREEN
          && vis(60, 180) == C_BLUE && vis(180, 180) == C_WHITE;
        check(ok, "T08b quadrant colors exact (TL=R TR=G BL=B BR=W)", "");
    }

    /* ================= T09 pattern red_rect + full ================= */
    {
        uint64_t r = call_pattern("red_rect");
        int dec = res_decode(r);
        int ok = dec == 0 && ri("windows", -1) == 1
          && ri("bytes", -1) == 115200 && ri("fill_ms", -1) >= 0;
        check(ok, "T09 red_rect map (fb path, full frame)", "");

        ok = vis(69, 69) == C_BLACK && vis(70, 70) == C_RED
          && vis(169, 169) == C_RED && vis(170, 170) == C_BLACK
          && vis(0, 0) == C_BLACK && vis(239, 239) == C_BLACK
          && vis(119, 119) == C_RED && vis(50, 70) == C_BLACK
          && vis(70, 50) == C_BLACK && vis(120, 120) == C_RED;
        check(ok, "T09b red 100x100 at (70,70) exact boundaries", "");

        r = call_pattern("full");
        dec = res_decode(r);
        ok = dec == 0 && ri("windows", -1) == 1
          && vis(0, 0) == C_WHITE && vis(239, 239) == C_WHITE
          && vis(120, 120) == C_WHITE;
        check(ok, "T09c pattern full = solid white frame", "");
    }

    /* ================= T10 blit(0) re-streams the module fb ============ */
    {
        reset_state(0);
        call_init("");
        call_int_fn(st_clear, 1, 0x1234);
        call_pattern("border");          /* partial windows: fb unchanged */
        uint64_t r = call_int_fn(st_blit, 0, 0);   /* no-arg -> default 0 */
        int dec = res_decode(r);
        int ok = dec == 0 && ri("ok", 0) == 1 && ri("bytes", -1) == 115200
          && ri("chunks", -1) == 29 && kidx("fill_ms") < 0;
        check(ok, "T10 blit() default 0 = module fb; no fill_ms key", "");

        ok = vis(0, 0) == 0x1234 && vis(239, 239) == 0x1234
          && vis(100, 100) == 0x1234;
        check(ok, "T10b panel back to the fb image (border overwritten)", "");
    }

    /* ================= T11 blit(buf_off) + bounds ================= */
    {
        /* stage a checker-ish frame at a valid nonzero offset */
        uint32_t off = 0x20000;                     /* 131072 */
        uint8_t *sb = arena + off;
        for (uint32_t i = 0; i < 57600; i++) {      /* wire order hi,lo */
            uint16_t px = (uint16_t)((i & 1) ? 0x0AA0 : 0xA0A0);
            sb[2 * i] = (uint8_t)(px >> 8);
            sb[2 * i + 1] = (uint8_t)px;
        }
        uint64_t r = call_int_fn(st_blit, 1, (int64_t)off);
        int dec = res_decode(r);
        int ok = dec == 0 && ri("bytes", -1) == 115200
          && vis(0, 0) == 0xA0A0 && vis(1, 0) == 0x0AA0
          && vis(239, 239) == 0x0AA0;   /* i = 57599 is odd */
        check(ok, "T11 blit(buf_off) streams caller-staged frame", "");

        r = call_int_fn(st_blit, 1, 4);             /* < 8 */
        ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "blit offset out of bounds") == 0;
        check(ok, "T11b blit off<8 refused (error map)", "");
        r = call_int_fn(st_blit, 1, 147945);        /* off+fb > LIN_END */
        ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "blit offset out of bounds") == 0;
        check(ok, "T11c blit off+115200>262144 refused", "");
        r = call_int_fn(st_blit, 1, -1);
        ok = res_decode(r) == 0 && rstr("error");
        check(ok, "T11d blit negative offset refused", "");
    }

    /* ================= T12 pattern kind domain ================= */
    {
        uint64_t r = call_pattern("checker");
        int ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "unknown pattern kind") == 0;
        check(ok, "T12a unknown kind -> error map",
              rstr("error") ? rstr("error") : "(none)");
        /* no-arg pattern: fixarray(0) -> str decode fails -> error */
        uint8_t abuf[2] = { 0x90, 0 };
        uint32_t aoff = host_write_args(abuf, 1);
        r = st_pattern(aoff, 1);
        ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "pattern kind required") == 0;
        check(ok, "T12b missing kind -> error map", "");
    }

    /* ================= T13 deinit + re-init state machine ============== */
    {
        ev_reset();
        uint64_t r = call_noargs(st_deinit);
        int dec = res_decode(r);
        int ci = ev_cmd(0, &cmd);
        int ok = dec == 0 && ri("ok", 0) == 1 && ci >= 0
          && evs[ci].bytes[0] == ST_CMD_DISPOFF
          && evs[ci + 1].type == 'D' && evs[ci + 1].a == 20
          && ev_cmd(1, &cmd) >= 0 && cmd == ST_CMD_SLPIN;
        check(ok, "T13 deinit: DISPOFF + 20ms + SLPIN", "");

        r = call_int_fn(st_clear, 1, 0);
        ok = res_decode(r) == 0 && rstr("error")
          && strcmp(rstr("error"), "not initialized") == 0;
        check(ok, "T13b clear after deinit refused", "");

        r = call_init("");                          /* re-init works */
        ok = res_decode(r) == 0 && ri("ok", 0) == 1;
        r = call_int_fn(st_clear, 1, 0x001F);
        ok = ok && res_decode(r) == 0 && ri("ok", 0) == 1
          && vis(120, 120) == C_BLUE;
        check(ok, "T13c re-init restores the draw path", "");
    }

    /* ================= T14 spi slot conflict (Review Focus #4) ======
     * the native face of the board vector: slot held by another owner
     * -> the host refusal raises through init as class=rt */
    {
        reset_state(0);
        slot_held = 1;
        EXPECT_TRAP(call_init(""), "T14 spi slot held -> init raises "
                    "xuansi.spi_config refusal");
        slot_held = 0;
        uint64_t r = call_init("");
        check(res_decode(r) == 0 && ri("ok", 0) == 1,
              "T14b init succeeds after the slot is released", "");
    }

    /* ================= T15 args-slot budget (§3.1) =================
     * 1000 cfg'd calls must not grow the bump arena (border pattern:
     * small SPI traffic, panel model absorbs the writes) */
    {
        reset_state(0);
        call_init("");
        uint32_t hw0 = heap_off;
        uint64_t r = 0;
        for (int i = 0; i < 1000; i++) {
            ev_reset();
            r = call_init("x_offset=i:0");          /* cfg'd call */
        }
        int dec = res_decode(r);
        snprintf(detail, sizeof detail, "hw %u->%u dec=%d ok=%ld",
                 hw0, heap_off, dec, ri("ok", -1));
        check(dec == 0 && ri("ok", -1) == 1 && heap_off == hw0,
              "T15 1000 cfg'd init calls: alloc high-water stable", detail);
    }

    printf("---- test_st7789: %d checks, %d fail ----\n", checks, fails);
    return fails ? 1 : 0;
}
