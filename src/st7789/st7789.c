/*
 * st7789.c — ST7789V 240x240 TFT panel driver module (kind=driver,
 * B10-T6; T7's closed loop consumes this Lua face verbatim).
 *
 * Canonical home: wan-cloud/xuansi-drivers (src/st7789/st7789.c).
 *
 * Board facts (owner-wired 2026-11-07, backlight verified lit):
 *   SCL(SCK)=41  SDA(MOSI)=42  RES=47  DC=21  BLK->3V3 (always lit)
 *   NO CS — the SPI bus is dedicated to the panel, which is exactly the
 *   abi=1 CS-less engine (spi_config no_cs=1 is the only legal value;
 *   no_cs=0 is refused by the host).
 *
 * Calling face (module_abi=1 marshalling; one msgpack array in, one
 * msgpack map result out):
 *
 *   bench.wasm.call("st7789", "init", cfg?)    cfg keys all optional,
 *                                              unknown keys ignored:
 *     x_offset  int   panel glass start offset, clamped to 0..80
 *                     (default 0 — the B10-T6 border probe on this
 *                     panel pinned 0). Despite the historical key name,
 *                     at MADCTL=0x00 the offset lands on RASET — see
 *                     the geometry note below.
 *     mode      int   SPI mode, clamped to 0..3 (default 3 — the ST7789
 *                     4-line interface samples MOSI on the rising SCK
 *                     edge with SCK idle high: CPOL=1/CPHA=2 = mode 3).
 *     invon     bool  INVON (display inversion) at init (default true —
 *                     the common 240x240 IPS glass needs it for true
 *                     colors; eyeball-tunable without rebuilding).
 *     -> {ok=1, x_offset, mode, invon, ms}
 *
 *   bench.wasm.call("st7789", "clear", color565?)   (default 0 = black)
 *     -> {ok=1, bytes, chunks, fill_ms, xfer_ms, total_ms}
 *
 *   bench.wasm.call("st7789", "blit", buf_off?)     (default 0)
 *     Streams one full frame (115,200B RGB565, hi byte first) from the
 *     module's linear memory at buf_off to the panel. buf_off 0 (or
 *     absent) = the module's own framebuffer (re-stream the last
 *     fb-path image — the pure-SPI timing path). Nonzero buf_off must
 *     satisfy 8 <= buf_off and buf_off + 115200 <= 262144 (the frozen
 *     initial linear memory); violations return {error=...} rather
 *     than trapping.
 *     -> {ok=1, bytes, chunks, xfer_ms, total_ms}   (fill_ms = 0)
 *
 *   bench.wasm.call("st7789", "pattern", kind)
 *     kind in "full" | "border" | "quad" | "red_rect":
 *       full      solid white, full frame (fb path — fill + stream)
 *       border    8px white frame at the panel edge, drawn as four
 *                 partial-window strips (offset probe: border flush to
 *                 the glass edge = x_offset correct; inset ~80 rows =
 *                 the wrong candidate)
 *       quad      four 120x120 quadrants red/green/blue/white (partial
 *                 windows; orientation/RGB-order eyeball)
 *       red_rect  black frame + centered 100x100 red rect at (70,70)
 *                 (fb path; camera-contrast eyeball)
 *     -> {ok=1, kind, windows, bytes, chunks, fill_ms, xfer_ms, total_ms}
 *     (fb paths carry a real fill_ms; partial-window patterns report
 *     fill_ms 0 — their tiny stage fills are counted in xfer_ms.)
 *
 *   bench.wasm.call("st7789", "deinit")
 *     DISPOFF + SLPIN (panel to sleep). The SPI slot and the RES/DC
 *     pin claims REMAIN — abi=1 has no release host fn; they are
 *     released by session_reset(loader session) / vm.reopen, like
 *     every wasm claim.
 *     -> {ok=1, ms}
 *
 *   Draw ops before a successful init return {error="not initialized"};
 *   domain errors return {error=...} maps (clean in-domain refusals —
 *   host-level refusals, e.g. the spi-slot owner conflict, surface as
 *   the host's wasm exception, class=rt).
 *
 * Controller geometry (drives the offset semantics): the ST7789V RAM is
 * 240 sources (CASET x, 0..239) x 320 gates (RASET y, 0..319). A 240x240
 * glass bonds all 240 sources and 240 of the 320 gates, so the
 * glass-specific start offset lands on the GATE (RASET) axis at MADCTL
 * 0x00 portrait: RASET runs off..off+239, CASET stays 0..239. The brief
 * freezes the cfg key name `x_offset`; it IS this glass offset. (The
 * "x offset 80" lore from landscape-default libraries is the same
 * offset seen after an MX|MV rotation swaps the axes.)
 *
 * Timing: draw ops wrap the fill loop (fill_ms) and the display phase
 * (xfer_ms — window commands + RAMWR + chunked streams) with millis();
 * total_ms spans both. Under fast-interp the wasm-side fill loop
 * dominates; the SPI engine moves the bytes DMA-side. Data streams are
 * chunked at the engine's 4096B per-xfer cap (full frame = 29 chunks).
 *
 * Linear memory: fb 115,200 + stage 4,096 + out 2,048 + heap 8,192 +
 * args slot 128 + cmd 8 ~= 132KB + wasm-ld 64KB stack -> the frozen
 * default 262,144B initial (4 pages, max 1MB) — hello's budget class.
 */

#include <stdint.h>
#include <stddef.h>

#include "xuansi_host.h"

#ifndef XUANSI_ABI_OVERRIDE
#define XUANSI_ABI_OVERRIDE XUANSI_ABI
#endif

/* ---- native-test seam (same discipline as hello.c / ledwatch.c) ----
 * Unit tests compile this file with the host toolchain against a stub
 * host that ALSO models the panel (a 240x320 gate-source RAM with a
 * CASET/RASET/RAMWR interpreter), so window geometries are asserted on
 * the pixels the panel would actually show. The test owns the "linear
 * memory" arena; every host-touched buffer lives at a fixed arena
 * offset. The wasm32 build pre-processes the seam away. */

#ifdef XUANSI_NATIVE_TEST
uint8_t *xuansi_lin_base;
#define XUANSI_IN(off) ((const uint8_t *)((size_t)xuansi_lin_base + (size_t)(uint32_t)(off)))
#define XUANSI_OFF(p)  ((uint32_t)((size_t)(p) - (size_t)xuansi_lin_base))
#define XUANSI_EXPORT(name) /* native: plain symbol, no wasm attribute */
#define LIN_OUT_OFF    0x4000u             /* result marshal buffer */
#define LIN_ARGS_OFF   0x6000u             /* reused args slot (§3.1) */
#define LIN_CMD_OFF    0x6080u             /* 8B command / window tx */
#define LIN_STAGE_OFF  0x6100u             /* 4KB solid-fill staging */
#define LIN_FB_OFF     0x10000u            /* 240*240*2 framebuffer */
void xuansi_native_init(uint8_t *base);
#else
#define XUANSI_IN(off) ((const uint8_t *)(uintptr_t)(off))
#define XUANSI_OFF(p)  ((uint32_t)(uintptr_t)(p))
#define XUANSI_EXPORT(name) __attribute__((export_name(name)))
#endif

/* ---- panel + bus constants (board facts / ST7789V conventions) ---- */

#define PIN_SCLK   41u          /* SCL */
#define PIN_MOSI   42u          /* SDA */
#define PIN_RES    47u
#define PIN_DC     21u
#define SPI_SLOT   0u           /* SPI2 */
#define SPI_HZ     40000000u    /* brief target; engine cap 80MHz */
#define DEF_MODE   3u           /* CPOL=1/CPHA=2 — ST7789 4-line sample edge */
#define DEF_GATE_OFF 0u         /* B10-T6 border probe verdict on this glass */

#define PANEL_W    240u
#define PANEL_H    240u
#define FB_BYTES   (PANEL_W * PANEL_H * 2u)     /* 115,200 */
#define XFER_CAP   4096u                        /* engine per-xfer cap */
#define LIN_END    262144u                      /* frozen initial memory */

#define ST_CMD_SLPOUT  0x11
#define ST_CMD_COLMOD  0x3A
#define ST_CMD_MADCTL  0x36
#define ST_CMD_INVON   0x21
#define ST_CMD_CASET   0x2A
#define ST_CMD_RASET   0x2B
#define ST_CMD_RAMWR   0x2C
#define ST_CMD_DISPON  0x29
#define ST_CMD_DISPOFF 0x28
#define ST_CMD_SLPIN   0x10
#define MADCTL_VAL     0x00       /* portrait, RGB order (no swap bits) */
#define COLMOD_16BIT   0x55

#define BORDER_T   8u
#define QUAD_HALF  120u
#define RECT_XY    70u
#define RECT_WH    100u

/* solid colors (RGB565) */
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
#define C_RED   0xF800u
#define C_GREEN 0x07E0u
#define C_BLUE  0x001Fu

/* ---- in-linear-memory buffers (bump allocator; offset 0 reserved) ----
 * fix1 args-slot convention (module-ABI.md §3.1): small allocations
 * (<= ARGS_SLOT_SZ) reuse one fixed slot; the fw host serializes every
 * wasm call (s_wasm_mu) so the reuse is race-free. Larger allocations
 * keep the bump arena (one-shot scale). */

#define HEAP_SZ 8192
#define ARGS_SLOT_SZ 128
#ifdef XUANSI_NATIVE_TEST
static uint32_t heap_off = 8;
static uint8_t *args_slot;                 /* placed by xuansi_native_init() */
static uint8_t *cmd_buf;                   /* 8B: command / window words */
static uint8_t *stage_buf;                 /* 4096B solid-fill staging */
static uint8_t *st_fb;                     /* 115,200B framebuffer */
static uint8_t *out_buf;                   /* result marshal buffer */
#else
static uint8_t heap_buf[HEAP_SZ] __attribute__((aligned(8), unused));
static uint8_t args_slot[ARGS_SLOT_SZ] __attribute__((aligned(8)));
static uint8_t cmd_buf[8] __attribute__((aligned(8)));
static uint8_t stage_buf[XFER_CAP] __attribute__((aligned(8)));
static uint8_t st_fb[FB_BYTES] __attribute__((aligned(8)));
static uint8_t out_buf[2048] __attribute__((aligned(8)));
static uint32_t heap_off = 8;
#endif

/* module state */
static uint32_t st_gate_off = DEF_GATE_OFF; /* glass offset, gate axis */
static uint32_t st_mode = DEF_MODE;
static uint32_t st_invon = 1;
static int      st_inited;

/* timing landing spots (linear .bss — the host writes them; wasm stack
 * slots sit on the execution page, out of the host's reach: §2.1) */
static uint32_t g_t0, g_t1, g_t2, g_n, g_spi_rc;

XUANSI_EXPORT("xuansi_alloc")
uint32_t xuansi_alloc(uint32_t n)
{
    if (n == 0 || n > HEAP_SZ) return 0;
    if (n <= ARGS_SLOT_SZ) return XUANSI_OFF(args_slot);   /* §3.1 reuse */
    uint32_t off = (heap_off + 7u) & ~(uint32_t)7u;
    if (off + n > HEAP_SZ) return 0;
    heap_off = off + n;
    return off;
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

/* ---- minimal msgpack face (int family decode; fixstr value decode;
 * fixmap + int/str encode — the abi=1 value domain these calls need) ---- */

static int mp_read_int(const uint8_t *p, uint32_t len, uint32_t *pos,
                       int64_t *out)
{
    if (*pos >= len) return -1;
    uint8_t t = p[(*pos)++];
    if ((t & 0x80) == 0) { *out = t; return 0; }
    if (t >= 0xe0) { *out = (int8_t)t; return 0; }
    uint32_t n = 0;
    int sign = 0;
    if (t == 0xcc) n = 1;
    else if (t == 0xcd) n = 2;
    else if (t == 0xce) n = 4;
    else if (t == 0xd0) { n = 1; sign = 1; }
    else if (t == 0xd1) { n = 2; sign = 1; }
    else if (t == 0xd2) { n = 4; sign = 1; }
    else return -1;
    if (*pos + n > len) return -1;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = (v << 8) | p[(*pos)++];
    if (sign) {                                 /* two's complement */
        uint64_t m = (1ull << (8u * n)) - 1ull;
        *out = (int64_t)(v & m);
        if (*out & (int64_t)1 << (8u * n - 1u))
            *out -= (int64_t)(m + 1ull);
    } else *out = (int64_t)v;
    return 0;
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

static int64_t clamp_i64(int64_t v, int64_t lo, int64_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* args[0] as int (tolerant default, hello's arg_int discipline) */
static int64_t arg_int0(const uint8_t *p, uint32_t len, int64_t dflt)
{
    uint32_t pos = 0;
    if (p == NULL || pos >= len) return dflt;
    uint8_t t = p[pos++];
    if ((t & 0xf0) == 0x90 && (t & 0x0f) >= 1) {
        int64_t v;
        if (mp_read_int(p, len, &pos, &v) == 0) return v;
    }
    return dflt;
}

/* args[0] as str (fixstr or str8; returns length or -1) */
static int arg_str0(const uint8_t *p, uint32_t len, char *out, uint32_t cap)
{
    uint32_t pos = 0;
    if (p == NULL || pos >= len) return -1;
    uint8_t t = p[pos++];
    if ((t & 0xf0) != 0x90 || (t & 0x0f) < 1) return -1;
    if (pos >= len) return -1;
    t = p[pos];
    uint32_t n;
    if ((t & 0xe0) == 0xa0) { n = t & 0x1f; pos++; }   /* fixstr */
    else if (t == 0xd9) {                              /* str8 */
        pos++;
        if (pos >= len) return -1;
        n = p[pos++];
    } else return -1;
    if (pos + n > len || n >= cap) return -1;
    for (uint32_t i = 0; i < n; i++) out[i] = (char)p[pos + i];
    out[n] = 0;
    return (int)n;
}

/* fixstr key == literal? (length already decoded by the caller) */
static int key_is(const char *key, uint32_t klen, const char *lit)
{
    uint32_t i = 0;
    while (i < klen && lit[i] && key[i] == lit[i]) i++;
    return i == klen && lit[i] == 0;
}

/* decode cfg map from args[0] (tolerant: malformed input keeps the
 * defaults; unknown keys ignored — forward compat; values clamped) */
static void cfg_decode(const uint8_t *p, uint32_t len,
                       int64_t *x_offset, int64_t *mode, int *invon)
{
    *x_offset = (int64_t)DEF_GATE_OFF;
    *mode = (int64_t)DEF_MODE;
    *invon = 1;
    if (p == NULL || len == 0) return;
    uint32_t pos = 0;
    uint8_t t = p[pos++];
    uint32_t n;
    if ((t & 0xf0) == 0x90) n = t & 0x0f;            /* fixarray */
    else if (t == 0xdc) {                            /* array16 */
        if (pos + 2 > len) return;
        n = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return;
    if (n < 1) return;
    if (pos >= len) return;
    t = p[pos++];
    uint32_t entries;
    if ((t & 0xf0) == 0x80) entries = t & 0x0f;      /* fixmap */
    else if (t == 0xde) {                            /* map16 */
        if (pos + 2 > len) return;
        entries = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return;

    char key[32];
    for (uint32_t e = 0; e < entries; e++) {
        if (pos >= len) return;
        uint8_t kt = p[pos];
        if ((kt & 0xe0) != 0xa0) return;             /* key: fixstr */
        uint32_t klen = kt & 0x1f;
        if (pos + 1 + klen > len || klen >= sizeof key) return;
        pos++;
        for (uint32_t i = 0; i < klen; i++) key[i] = (char)p[pos + i];
        pos += klen;
        key[klen] = 0;
        if (pos >= len) return;
        uint8_t vt = p[pos];
        if (vt == 0xc3 || vt == 0xc2) {              /* bool */
            pos++;
            if (key_is(key, klen, "invon")) *invon = (vt == 0xc3);
            continue;
        }
        int64_t iv = 0;
        if (mp_read_int(p, len, &pos, &iv) != 0) return;
        if (key_is(key, klen, "x_offset")) *x_offset = iv;
        else if (key_is(key, klen, "mode")) *mode = iv;
        /* unknown int keys: ignored */
    }
}

/* ---- result encoding: one fixmap of int/str values ---- */

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

/* ---- panel command layer ---- */

/* one command byte, DC low */
static void st_cmd(uint8_t c)
{
    cmd_buf[0] = c;
    gpio_set(PIN_DC, 0);
    g_n = 0;
    spi_xfer(&g_n, SPI_SLOT, XUANSI_OFF(cmd_buf), 1u, 0u, 0u);
}

/* len bytes from linear offset off, DC high; engine cap 4096B/xfer.
 * Returns the chunk count. */
static uint32_t st_data(uint32_t off, uint32_t len)
{
    uint32_t chunks = 0, done = 0;
    gpio_set(PIN_DC, 1);
    while (done < len) {
        uint32_t n = len - done;
        if (n > XFER_CAP) n = XFER_CAP;
        g_n = 0;
        spi_xfer(&g_n, SPI_SLOT, off + done, n, 0u, 0u);
        done += n;
        chunks++;
    }
    return chunks;
}

/* address window: x = source axis (0..239, no offset), y = gate axis
 * (the glass start offset is added here — see the geometry note) */
static void st_win(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t ya = y0 + st_gate_off, yb = y1 + st_gate_off;
    st_cmd(ST_CMD_CASET);
    cmd_buf[0] = (uint8_t)(x0 >> 8);  cmd_buf[1] = (uint8_t)x0;
    cmd_buf[2] = (uint8_t)(x1 >> 8);  cmd_buf[3] = (uint8_t)x1;
    st_data(XUANSI_OFF(cmd_buf), 4u);
    st_cmd(ST_CMD_RASET);
    cmd_buf[0] = (uint8_t)(ya >> 8);  cmd_buf[1] = (uint8_t)ya;
    cmd_buf[2] = (uint8_t)(yb >> 8);  cmd_buf[3] = (uint8_t)yb;
    st_data(XUANSI_OFF(cmd_buf), 4u);
}

/* fill the 4KB stage with a solid color (once per window color) —
 * WIRE ORDER, hi byte first (see fb_fill) */
static void stage_fill(uint16_t color)
{
    uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)color;
    for (uint32_t i = 0; i < XFER_CAP; i += 2u) {
        stage_buf[i] = hi; stage_buf[i + 1u] = lo;
    }
}

/* per-op stream byte tally (st_fill_win / fb_stream accumulate) */
static uint32_t g_bytes;

/* stream a solid color into a window directly from the stage (no fb).
 * The stage is a REPEATING 4KB pattern — every chunk re-streams from
 * the stage BASE (an advancing offset would read past the 4KB buffer
 * into arena zeros); returns the chunk count. */
static uint32_t st_fill_win(uint16_t color, uint32_t x0, uint32_t y0,
                            uint32_t x1, uint32_t y1)
{
    stage_fill(color);
    st_win(x0, y0, x1, y1);
    st_cmd(ST_CMD_RAMWR);
    uint32_t total = (x1 - x0 + 1u) * (y1 - y0 + 1u) * 2u;
    g_bytes += total;
    uint32_t chunks = 0, done = 0;
    while (done < total) {
        uint32_t n = total - done;
        if (n > XFER_CAP) n = XFER_CAP;
        chunks += st_data(XUANSI_OFF(stage_buf), n);
        done += n;
    }
    return chunks;
}

/* full-frame stream from a validated linear offset (fill phase already
 * done by the caller when applicable); fills the timing statics */
static uint32_t fb_stream(uint32_t off)
{
    millis(&g_t1);
    st_win(0, 0, PANEL_W - 1u, PANEL_H - 1u);
    st_cmd(ST_CMD_RAMWR);
    g_bytes += FB_BYTES;
    uint32_t chunks = st_data(off, FB_BYTES);
    millis(&g_t2);
    return chunks;
}

/* fb fill / rect overlay: WIRE ORDER (RGB565 hi byte first) — the fb
 * and any blit(buf_off) source are byte streams in the panel's wire
 * order, the same order the GC2145 camera delivers RGB565 frames in
 * (ledwatch's hi-byte-first finding) — a camera frame can therefore be
 * blitted verbatim. u16 stores would be little-endian on the wire. */
static void fb_fill(uint16_t color)
{
    uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)color;
    for (uint32_t i = 0; i < FB_BYTES; i += 2u) {
        st_fb[i] = hi; st_fb[i + 1u] = lo;
    }
}

static void fb_rect(uint16_t color, uint32_t x0, uint32_t y0,
                    uint32_t w, uint32_t h)
{
    uint8_t hi = (uint8_t)(color >> 8), lo = (uint8_t)color;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = (y0 + y) * PANEL_W + x0;
        for (uint32_t x = 0; x < w; x++) {
            st_fb[(row + x) * 2u] = hi;
            st_fb[(row + x) * 2u + 1u] = lo;
        }
    }
}

/* ---- exported face ---- */

XUANSI_EXPORT("init")
uint64_t st_init(uint32_t ptr, uint32_t len)
{
    int64_t xo, mode;
    int invon;
    cfg_decode(XUANSI_IN(ptr), len, &xo, &mode, &invon);
    xo = clamp_i64(xo, 0, 80);
    mode = clamp_i64(mode, 0, 3);

    millis(&g_t0);
    g_spi_rc = 7;
    /* CS-less (no_cs=1) is the only abi=1 bus — matches this panel
     * (no CS pin, dedicated bus). A slot/pin conflict raises the host
     * exception (class=rt) before rc can be consulted. */
    spi_config(&g_spi_rc, SPI_SLOT, PIN_SCLK, SPI_HZ, (int32_t)PIN_MOSI, -1,
               (uint32_t)mode, 1u);
    if (g_spi_rc != 0) return err_result("spi_config refused");  /* defensive */
    st_gate_off = (uint32_t)xo;
    st_mode = (uint32_t)mode;
    st_invon = invon ? 1u : 0u;

    /* hardware reset: RES low 10ms (datasheet minimum is 10us; 10ms is
     * the conservative convention), then the 120ms power-up wait and
     * the 120ms sleep-out recovery */
    gpio_set(PIN_RES, 0);
    delay_ms(10);
    gpio_set(PIN_RES, 1);
    delay_ms(120);
    st_cmd(ST_CMD_SLPOUT);
    delay_ms(120);
    st_cmd(ST_CMD_COLMOD);
    cmd_buf[0] = COLMOD_16BIT;
    st_data(XUANSI_OFF(cmd_buf), 1u);
    st_cmd(ST_CMD_MADCTL);
    cmd_buf[0] = MADCTL_VAL;
    st_data(XUANSI_OFF(cmd_buf), 1u);
    if (st_invon) st_cmd(ST_CMD_INVON);
    st_win(0, 0, PANEL_W - 1u, PANEL_H - 1u);
    st_cmd(ST_CMD_DISPON);
    millis(&g_t2);
    st_inited = 1;

    enc_t e;
    enc_begin(&e);
    enc_int(&e, "ok", 1);
    enc_int(&e, "x_offset", st_gate_off);
    enc_int(&e, "mode", st_mode);
    enc_int(&e, "invon", st_invon);
    enc_int(&e, "ms", g_t2 - g_t0);
    return enc_end(&e);
}

XUANSI_EXPORT("clear")
uint64_t st_clear(uint32_t ptr, uint32_t len)
{
    if (!st_inited) return err_result("not initialized");
    int64_t c = clamp_i64(arg_int0(XUANSI_IN(ptr), len, 0), 0, 0xFFFF);
    uint16_t color = (uint16_t)c;

    g_bytes = 0;
    millis(&g_t0);
    fb_fill(color);
    uint32_t chunks = fb_stream(XUANSI_OFF(st_fb));

    enc_t e;
    enc_begin(&e);
    enc_int(&e, "ok", 1);
    enc_int(&e, "bytes", g_bytes);
    enc_int(&e, "chunks", chunks);
    enc_int(&e, "fill_ms", g_t1 - g_t0);
    enc_int(&e, "xfer_ms", g_t2 - g_t1);
    enc_int(&e, "total_ms", g_t2 - g_t0);
    return enc_end(&e);
}

XUANSI_EXPORT("blit")
uint64_t st_blit(uint32_t ptr, uint32_t len)
{
    if (!st_inited) return err_result("not initialized");
    int64_t off = arg_int0(XUANSI_IN(ptr), len, 0);
    if (off < 0) return err_result("blit offset out of bounds");
    if (off != 0 && ((uint64_t)off < 8u
                     || (uint64_t)off + FB_BYTES > LIN_END))
        return err_result("blit offset out of bounds");

    g_bytes = 0;
    millis(&g_t0);
    uint32_t chunks = fb_stream(off == 0 ? XUANSI_OFF(st_fb)
                                         : (uint32_t)off);

    enc_t e;
    enc_begin(&e);
    enc_int(&e, "ok", 1);
    enc_int(&e, "bytes", g_bytes);
    enc_int(&e, "chunks", chunks);
    enc_int(&e, "xfer_ms", g_t2 - g_t0);
    enc_int(&e, "total_ms", g_t2 - g_t0);
    return enc_end(&e);
}

XUANSI_EXPORT("pattern")
uint64_t st_pattern(uint32_t ptr, uint32_t len)
{
    if (!st_inited) return err_result("not initialized");
    char kind[16];
    if (arg_str0(XUANSI_IN(ptr), len, kind, sizeof kind) < 0)
        return err_result("pattern kind required");

    uint32_t windows, chunks = 0;
    g_t1 = 0;                                   /* fill_ms default 0 */
    g_bytes = 0;

    if (streq(kind, "full")) {
        millis(&g_t0);
        fb_fill(C_WHITE);
        chunks = fb_stream(XUANSI_OFF(st_fb));
        windows = 1;
    } else if (streq(kind, "border")) {
        /* four 8px strips around the glass edge (partial windows; the
         * interior keeps its previous content — the offset probe draws
         * these over a cleared screen) */
        millis(&g_t0);
        g_t1 = g_t0;                            /* no fb fill phase */
        chunks += st_fill_win(C_WHITE, 0, 0, PANEL_W - 1u, BORDER_T - 1u);
        chunks += st_fill_win(C_WHITE, 0, PANEL_H - BORDER_T,
                              PANEL_W - 1u, PANEL_H - 1u);
        chunks += st_fill_win(C_WHITE, 0, BORDER_T, BORDER_T - 1u,
                              PANEL_H - BORDER_T - 1u);
        chunks += st_fill_win(C_WHITE, PANEL_W - BORDER_T, BORDER_T,
                              PANEL_W - 1u, PANEL_H - BORDER_T - 1u);
        millis(&g_t2);
        windows = 4;
    } else if (streq(kind, "quad")) {
        millis(&g_t0);
        g_t1 = g_t0;
        chunks += st_fill_win(C_RED,   0,          0,
                              QUAD_HALF - 1u, QUAD_HALF - 1u);
        chunks += st_fill_win(C_GREEN, QUAD_HALF,  0,
                              PANEL_W - 1u,  QUAD_HALF - 1u);
        chunks += st_fill_win(C_BLUE,  0,          QUAD_HALF,
                              QUAD_HALF - 1u, PANEL_H - 1u);
        chunks += st_fill_win(C_WHITE, QUAD_HALF,  QUAD_HALF,
                              PANEL_W - 1u,  PANEL_H - 1u);
        millis(&g_t2);
        windows = 4;
    } else if (streq(kind, "red_rect")) {
        millis(&g_t0);
        fb_fill(C_BLACK);
        fb_rect(C_RED, RECT_XY, RECT_XY, RECT_WH, RECT_WH);
        chunks = fb_stream(XUANSI_OFF(st_fb));
        windows = 1;
    } else {
        return err_result("unknown pattern kind");
    }

    enc_t e;
    enc_begin(&e);
    enc_int(&e, "ok", 1);
    enc_str(&e, "kind", kind);
    enc_int(&e, "windows", windows);
    enc_int(&e, "bytes", g_bytes);
    enc_int(&e, "chunks", chunks);
    enc_int(&e, "fill_ms", g_t1 - g_t0);
    enc_int(&e, "xfer_ms", g_t2 - g_t1);
    enc_int(&e, "total_ms", g_t2 - g_t0);
    return enc_end(&e);
}

XUANSI_EXPORT("deinit")
uint64_t st_deinit(uint32_t ptr, uint32_t len)
{
    (void)ptr; (void)len;
    if (!st_inited) return err_result("not initialized");
    millis(&g_t0);
    st_cmd(ST_CMD_DISPOFF);
    delay_ms(20);
    st_cmd(ST_CMD_SLPIN);
    millis(&g_t2);
    st_inited = 0;

    enc_t e;
    enc_begin(&e);
    enc_int(&e, "ok", 1);
    enc_int(&e, "ms", g_t2 - g_t0);
    return enc_end(&e);
}

/* ---- native seam init: place the host-touched buffers in the arena ---- */
#ifdef XUANSI_NATIVE_TEST
void xuansi_native_init(uint8_t *base)
{
    xuansi_lin_base = base;
    out_buf = base + LIN_OUT_OFF;
    args_slot = base + LIN_ARGS_OFF;
    cmd_buf = base + LIN_CMD_OFF;
    stage_buf = base + LIN_STAGE_OFF;
    st_fb = base + LIN_FB_OFF;
}
#endif
