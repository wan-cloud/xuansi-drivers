/*
 * ledwatch.c — LED on/off region detector, the ABI's first algorithm
 * module (kind=algorithm; T6 st7789 and T7 closed-loop consume this
 * analyze() output face verbatim).
 *
 * Canonical home: wan-cloud/xuansi-drivers (src/ledwatch/ledwatch.c).
 *
 * Calling face (module_abi=1 marshalling — one msgpack array in, one
 * msgpack result out; see README "Module ABI"):
 *
 *   bench.wasm.call("ledwatch", "analyze", cfg)
 *
 *   cfg (all keys optional; unknown keys are ignored — forward compat
 *   mirroring the op face):
 *     threshold      int   luma level counted as "lit", 0..255
 *                          (default 128; a pixel is lit when its luma
 *                          is >= threshold)
 *     min_area       int   regions with fewer pixels are dropped
 *                          (default 16)
 *     max_regions    int   cap on the returned list, 1..32 (default 8)
 *     discard_first  bool  consume one frame and throw it away before
 *                          analyzing (default true — the first frame
 *                          after camera-on carries unconverged AWB).
 *                          Stateless per call: discard_first=true makes
 *                          exactly two camera_frame_get calls.
 *
 *   Returns { regions = { {x,y,w,h,area,luma_mean,r_mean,g_mean,b_mean}, ... },
 *             frame   = { w = 320, h = 240 } }
 *   Regions are sorted by area, largest first, capped at max_regions.
 *   All means are integers 0..255 (the abi=1 value bridge carries no
 *   floats): 5/6-bit RGB565 components replicated into 8 bits, means
 *   rounded to nearest. luma is the ITU-R BT.601 integer approximation
 *   (77*R + 150*G + 29*B) >> 8, so pure red is luma 76 — callers
 *   hunting red-only LEDs pass a lower threshold.
 *
 *   The camera must already be on at QVGA (caller's responsibility,
 *   same contract as hello's cam_frame): camera not on, or a larger
 *   framesize, surfaces as the host's wasm exception (class=rt).
 *
 * Pipeline: RGB565 (hi byte first — the GC2145 DVP wire order) ->
 * BT.601 luma -> threshold -> two-pass connected-component labelling
 * (4-connectivity, union-find with path compression, u16 label map in
 * linear memory) -> per-label area -> top-N selection by area -> one
 * stats pass over the selected labels (bbox + luma/RGB means) ->
 * msgpack map result.
 *
 * Label-space proof (why u16 labels cannot overflow at QVGA): a label
 * is created only by a lit pixel whose left and up neighbours are dark
 * — such "starters" form a 4-independent set on the 320x240 grid, so
 * at most w*h/2 = 38400 labels exist; 38400 < 65535. The checkerboard
 * vector (38400 area-1 regions) pins the worst case.
 *
 * Linear-memory budget (QVGA): frame 153,600 + label map 153,600 +
 * parent 76,804 + area 153,608 + selected map 76,804 + heap 8,192 +
 * result 4,096 ~= 627KB, plus the wasm-ld default 64KB stack -> builds
 * pin --initial-memory=720896 (11 pages; max stays 1MB). One ledwatch
 * instance + one hello co-load fit the board's 1MB WAMR pool; two
 * ledwatch instances do not (the loader refuses with an out-of-memory
 * error — by design).
 */
#include <stdint.h>
#include <stddef.h>

#include "xuansi_host.h"

#ifndef XUANSI_ABI_OVERRIDE
#define XUANSI_ABI_OVERRIDE XUANSI_ABI
#endif

/* ---- native-test seam (same discipline as hello.c) ----------------
 * Unit tests compile this file with the host toolchain against a stub
 * host. The test owns a "linear memory" arena and passes u32 offsets
 * exactly like the wasm host does; the buffers the HOST touches
 * (frame buffer, result marshal buffer) live at fixed arena offsets.
 * Internal working arrays are ordinary statics in both builds. The
 * wasm32 build pre-processes this seam away. */

#ifdef XUANSI_NATIVE_TEST
uint8_t *xuansi_lin_base;
#define XUANSI_IN(off) ((const uint8_t *)((size_t)xuansi_lin_base + (size_t)(uint32_t)(off)))
#define XUANSI_OFF(p)  ((uint32_t)((size_t)(p) - (size_t)xuansi_lin_base))
#define XUANSI_EXPORT(name) /* native: plain symbol, no wasm attribute */
#define LIN_OUT_OFF    0x4000u             /* result marshal buffer */
#define LIN_ARGS_OFF   0x6000u             /* reused args slot (fix1) */
#define LIN_FRAME_OFF  0x10000u            /* QVGA frame buffer */
void xuansi_native_init(uint8_t *base);
#else
#define XUANSI_IN(off) ((const uint8_t *)(uintptr_t)(off))
#define XUANSI_OFF(p)  ((uint32_t)(uintptr_t)(p))
#define XUANSI_EXPORT(name) __attribute__((export_name(name)))
#endif

/* ---- in-linear-memory buffers (bump allocator; offset 0 reserved) ----
 * fix1 args-slot convention (module-ABI.md §3.1): the host calls
 * xuansi_alloc on EVERY call-with-args — a bump-only allocator is a hidden
 * per-call budget (~230 calls at cfg scale) that a polling module exhausts
 * in minutes. Small args (<= ARGS_SLOT_SZ) therefore reuse one fixed slot;
 * it is safe because the fw host serializes every wasm call (s_wasm_mu) —
 * a second call cannot be in flight while the slot's args are unconsumed.
 * Larger allocations keep the bump arena (one-shot scale). */
#define HEAP_SZ 8192
#define ARGS_SLOT_SZ 128
#ifdef XUANSI_NATIVE_TEST
static uint32_t heap_off = 8;
static uint8_t *args_slot;                 /* placed by xuansi_native_init() */
#else
static uint8_t heap_buf[HEAP_SZ] __attribute__((aligned(8), unused));
static uint8_t args_slot[ARGS_SLOT_SZ] __attribute__((aligned(8)));
static uint32_t heap_off = 8;
#endif

/* QVGA RGB565 frame buffer (320*240*2 = 153,600B; linear .bss, costs
 * no module file size). VGA frames raise over-cap at the host face. */
#define LW_W 320u
#define LW_H 240u
#define FRAME_CAP (LW_W * LW_H * 2u)
#ifdef XUANSI_NATIVE_TEST
static uint8_t *frame_buf;                 /* placed by xuansi_native_init() */
#else
static uint8_t frame_buf[FRAME_CAP] __attribute__((aligned(8)));
#endif

#ifdef XUANSI_NATIVE_TEST
static uint8_t *out_buf;
#else
static uint8_t out_buf[4096] __attribute__((aligned(8)));
#endif

/* ---- CCL working arrays (module-internal; statics in both builds) ---- */

#define NPIX (LW_W * LW_H)                        /* 76,800 */
#define LABEL_MAX (NPIX / 2u + 2u)                /* 38,402 — see header proof */

static uint16_t lw_labels[NPIX];                  /* resolved label map */
static uint16_t lw_parent[LABEL_MAX];             /* union-find parents */
static uint32_t lw_area[LABEL_MAX];               /* per-root pixel count */
static uint16_t lw_sel[LABEL_MAX];                /* root -> selected idx */

/* ---- defaults + clamps (frozen with the calling face) ---- */

#define DEF_THRESHOLD 128
#define DEF_MIN_AREA  16
#define DEF_MAX_REGIONS 8
#define MAX_REGIONS_CAP 32

XUANSI_EXPORT("xuansi_alloc")
uint32_t xuansi_alloc(uint32_t n)
{
    if (n == 0 || n > HEAP_SZ) return 0;
    if (n <= ARGS_SLOT_SZ) return XUANSI_OFF(args_slot);   /* fix1: reuse */
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

/* ---- minimal msgpack face ----
 * Decode: args array -> cfg map (fixmap/map16), keys fixstr, values
 * int (fixint/u8/u16/u32) or bool — exactly the abi=1 value domain a
 * Lua cfg table can produce. Encode: fixmap/fixarray/fixstr + the int
 * widths of hello's mp_put_int. */

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

/* ---- cfg ---- */

typedef struct {
    int64_t threshold;
    int64_t min_area;
    int64_t max_regions;
    int discard_first;
} lw_cfg_t;

static int64_t clamp_i64(int64_t v, int64_t lo, int64_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int lw_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void lw_cfg_defaults(lw_cfg_t *c)
{
    c->threshold = DEF_THRESHOLD;
    c->min_area = DEF_MIN_AREA;
    c->max_regions = DEF_MAX_REGIONS;
    c->discard_first = 1;
}

/* read one fixstr key into buf (<=31 chars); returns length or -1 */
static int rd_fixstr(const uint8_t *p, uint32_t len, uint32_t *pos,
                     char *buf, uint32_t cap)
{
    if (*pos >= len) return -1;
    uint8_t t = p[*pos];
    if ((t & 0xe0) != 0xa0) return -1;
    uint32_t n = t & 0x1f;
    if (*pos + 1 + n > len || n >= cap) return -1;
    (*pos)++;
    for (uint32_t i = 0; i < n; i++) buf[i] = (char)p[(*pos)++];
    buf[n] = 0;
    return (int)n;
}

/* Decode cfg from the args array at (p,len). Tolerant: empty/absent/
 * malformed input leaves the defaults in place; out-of-domain values
 * are clamped into the documented domain. */
static void lw_cfg_decode(const uint8_t *p, uint32_t len, lw_cfg_t *c)
{
    lw_cfg_defaults(c);
    if (p == NULL || len == 0) return;
    uint32_t pos = 0;
    uint8_t t;
    if (pos >= len) return;
    t = p[pos++];
    uint32_t n;
    if ((t & 0xf0) == 0x90) n = t & 0x0f;          /* fixarray */
    else if (t == 0xdc) {                          /* array16 */
        if (pos + 2 > len) return;
        n = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return;
    if (n < 1) return;                             /* [] -> defaults */
    /* element 0: the cfg map */
    if (pos >= len) return;
    t = p[pos++];
    uint32_t entries;
    if ((t & 0xf0) == 0x80) entries = t & 0x0f;    /* fixmap */
    else if (t == 0xde) {                          /* map16 */
        if (pos + 2 > len) return;
        entries = ((uint32_t)p[pos] << 8) | p[pos + 1]; pos += 2;
    } else return;                                 /* not a map -> defaults */

    char key[32];
    for (uint32_t e = 0; e < entries; e++) {
        int klen = rd_fixstr(p, len, &pos, key, sizeof key);
        if (klen < 0) return;
        if (pos >= len) return;
        uint8_t vt = p[pos];
        int64_t iv = 0;
        if (vt == 0xc3 || vt == 0xc2) {            /* bool */
            pos++;
            if (lw_streq(key, "discard_first"))
                c->discard_first = (vt == 0xc3);
            continue;                              /* other bool keys: ignored */
        }
        if (mp_read_int(p, len, &pos, &iv) != 0) return;  /* out of domain */
        if (lw_streq(key, "threshold")) c->threshold = iv;
        else if (lw_streq(key, "min_area")) c->min_area = iv;
        else if (lw_streq(key, "max_regions")) c->max_regions = iv;
        /* unknown int keys: ignored */
    }
    c->threshold = clamp_i64(c->threshold, 0, 255);
    c->min_area = clamp_i64(c->min_area, 0, (int64_t)NPIX);
    c->max_regions = clamp_i64(c->max_regions, 1, MAX_REGIONS_CAP);
}

/* ---- pixel math ----
 * RGB565, hi byte first (GC2145 DVP wire order): px = fb[2i]<<8 |
 * fb[2i+1]. Components replicated into 8 bits (31->255, 63->255). */

static inline uint8_t lw_r8(uint16_t px)
{
    uint8_t r5 = (uint8_t)((px >> 11) & 0x1f);
    return (uint8_t)((r5 << 3) | (r5 >> 2));
}

static inline uint8_t lw_g8(uint16_t px)
{
    uint8_t g6 = (uint8_t)((px >> 5) & 0x3f);
    return (uint8_t)((g6 << 2) | (g6 >> 4));
}

static inline uint8_t lw_b8(uint16_t px)
{
    uint8_t b5 = (uint8_t)(px & 0x1f);
    return (uint8_t)((b5 << 3) | (b5 >> 2));
}

static inline uint8_t lw_luma(uint16_t px)
{
    /* BT.601 integer approximation */
    return (uint8_t)((77u * lw_r8(px) + 150u * lw_g8(px) + 29u * lw_b8(px)) >> 8);
}

static inline uint16_t lw_px(const uint8_t *fb, uint32_t i)
{
    return (uint16_t)(((uint16_t)fb[2 * i] << 8) | (uint16_t)fb[2 * i + 1]);
}

/* ---- union-find ---- */

static uint16_t lw_find(uint16_t x)
{
    uint16_t r = x;
    while (lw_parent[r] != r) r = lw_parent[r];
    while (lw_parent[x] != r) {           /* path compression */
        uint16_t nx = lw_parent[x];
        lw_parent[x] = r;
        x = nx;
    }
    return r;
}

/* ---- exported face ---- */

/* camera_info landing spot: linear .bss (abi=1 frozen note — wasm
 * stack slots are on the execution page, out of the host's reach) */
static uint32_t lw_cam_info[3];

typedef struct {
    uint32_t x0, y0, x1, y1;              /* inclusive bbox */
    uint32_t area;
    uint32_t luma_sum, r_sum, g_sum, b_sum;
} lw_stat_t;

XUANSI_EXPORT("analyze")
uint64_t analyze(uint32_t ptr, uint32_t len)
{
    lw_cfg_t cfg;
    lw_cfg_decode(XUANSI_IN(ptr), len, &cfg);

    /* geometry (ro probe; the frame_get below raises honestly when the
     * camera is off or the framesize exceeds this module's QVGA cap) */
    camera_info(lw_cam_info);
    uint32_t w = lw_cam_info[0], h = lw_cam_info[1];

    /* AWB first-frame discard (stateless per call) */
    uint32_t flen = 0;
    camera_frame_get(&flen, XUANSI_OFF(frame_buf), FRAME_CAP);
    if (cfg.discard_first)
        camera_frame_get(&flen, XUANSI_OFF(frame_buf), FRAME_CAP);

    uint32_t pos = 0;
    uint32_t npx = w * h;
    if (w == 0 || h == 0 || w > LW_W || h > LW_H
        || flen != npx * 2u) {
        /* unreachable on the frozen host face (frame_get raises first);
         * defensive: well-formed empty result */
        out_buf[pos++] = 0x82;
        pos = mp_put_str(out_buf, pos, "regions");
        out_buf[pos++] = 0x90;                    /* [] */
        pos = mp_put_str(out_buf, pos, "frame");
        out_buf[pos++] = 0x82;
        pos = mp_put_int(out_buf, pos, w);
        pos = mp_put_int(out_buf, pos, h);
        return pack(XUANSI_OFF(out_buf), pos);
    }

    const uint8_t *fb = frame_buf;
    uint8_t thr = (uint8_t)cfg.threshold;

    /* pass 1: luma + threshold + provisional labels + unions */
    uint32_t next_label = 1;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t i = row + x;
            uint16_t lab = 0;
            if (lw_luma(lw_px(fb, i)) >= thr) {
                uint16_t l = (x > 0) ? lw_labels[i - 1] : 0;
                uint16_t u = (y > 0) ? lw_labels[i - w] : 0;
                if (l == 0 && u == 0) {
                    if (next_label >= LABEL_MAX) {  /* QVGA-proof: unreachable */
                        lw_labels[i] = 0;
                        continue;
                    }
                    lab = (uint16_t)next_label++;
                    lw_parent[lab] = lab;
                } else if (l == 0) {
                    lab = lw_find(u);
                } else if (u == 0) {
                    lab = lw_find(l);
                } else {
                    uint16_t rl = lw_find(l), ru = lw_find(u);
                    if (rl != ru) {
                        if (rl < ru) lw_parent[ru] = rl;
                        else lw_parent[rl] = ru;
                    }
                    lab = (rl < ru) ? rl : ru;
                }
            }
            lw_labels[i] = lab;
        }
    }

    /* pass 2: resolve roots in place + per-root area */
    for (uint32_t l = 1; l < next_label; l++) lw_area[l] = 0;
    for (uint32_t i = 0; i < npx; i++) {
        uint16_t lab = lw_labels[i];
        if (lab != 0) {
            uint16_t r = lw_find(lab);
            lw_labels[i] = r;
            lw_area[r]++;
        }
    }

    /* top-N selection by area (largest first), min_area filter.
     * fix1: a == 0 labels are NON-ROOTS (ghosts) — pass 2 accumulates
     * lw_area on roots only, so an unmerged-absorbed label keeps area 0.
     * The old filter (a < mina alone) admitted them at min_area=0, the
     * stats pass never touched them (no pixel resolves to a non-root),
     * and the encode divided by n=0 (wasm i32 div-zero trap; bbox
     * underflow besides). Skipping area-0 keeps min_area=0 as the honest
     * "no area filter" idiom. */
    uint32_t maxr = (uint32_t)cfg.max_regions;
    uint32_t mina = (uint32_t)cfg.min_area;
    uint16_t top_label[MAX_REGIONS_CAP];
    uint32_t top_area[MAX_REGIONS_CAP];
    uint32_t top_n = 0;
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t a = lw_area[l];
        if (a == 0 || a < mina) continue;
        if (top_n == maxr && a <= top_area[top_n - 1]) continue;
        uint32_t j = (top_n < maxr) ? top_n : maxr - 1;
        if (top_n < maxr) top_n++;
        while (j > 0 && top_area[j - 1] < a) {
            top_area[j] = top_area[j - 1];
            top_label[j] = top_label[j - 1];
            j--;
        }
        top_area[j] = a;
        top_label[j] = (uint16_t)l;
    }

    /* selected map + stats pass */
    for (uint32_t l = 1; l < next_label; l++) lw_sel[l] = 0xFFFF;
    static lw_stat_t stats[MAX_REGIONS_CAP];   /* linear .bss (large) */
    for (uint32_t k = 0; k < top_n; k++) {
        lw_sel[top_label[k]] = (uint16_t)k;
        stats[k].x0 = LW_W; stats[k].y0 = LW_H;
        stats[k].x1 = 0; stats[k].y1 = 0;
        stats[k].area = 0;
        stats[k].luma_sum = 0; stats[k].r_sum = 0;
        stats[k].g_sum = 0; stats[k].b_sum = 0;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t i = row + x;
            uint16_t lab = lw_labels[i];
            if (lab == 0) continue;
            uint16_t k16 = lw_sel[lab];
            if (k16 == 0xFFFF) continue;
            lw_stat_t *s = &stats[k16];
            if (x < s->x0) s->x0 = x;
            if (y < s->y0) s->y0 = y;
            if (x > s->x1) s->x1 = x;
            if (y > s->y1) s->y1 = y;
            s->area++;
            uint16_t px = lw_px(fb, i);
            s->luma_sum += lw_luma(px);
            s->r_sum += lw_r8(px);
            s->g_sum += lw_g8(px);
            s->b_sum += lw_b8(px);
        }
    }

    /* encode { regions = {...}, frame = {w, h} } */
    pos = 0;
    out_buf[pos++] = 0x82;                        /* fixmap(2) */
    pos = mp_put_str(out_buf, pos, "regions");
    if (top_n < 16) out_buf[pos++] = (uint8_t)(0x90 | top_n);
    else { out_buf[pos++] = 0xdc;                 /* array16 */
           out_buf[pos++] = (uint8_t)(top_n >> 8); out_buf[pos++] = (uint8_t)top_n; }
    for (uint32_t k = 0; k < top_n; k++) {
        lw_stat_t *s = &stats[k];
        uint32_t n = s->area;
        out_buf[pos++] = 0x89;                    /* fixmap(9) */
        pos = mp_put_str(out_buf, pos, "x");
        pos = mp_put_int(out_buf, pos, s->x0);
        pos = mp_put_str(out_buf, pos, "y");
        pos = mp_put_int(out_buf, pos, s->y0);
        pos = mp_put_str(out_buf, pos, "w");
        pos = mp_put_int(out_buf, pos, s->x1 - s->x0 + 1);
        pos = mp_put_str(out_buf, pos, "h");
        pos = mp_put_int(out_buf, pos, s->y1 - s->y0 + 1);
        pos = mp_put_str(out_buf, pos, "area");
        pos = mp_put_int(out_buf, pos, n);
        pos = mp_put_str(out_buf, pos, "luma_mean");
        pos = mp_put_int(out_buf, pos, (s->luma_sum + n / 2) / n);
        pos = mp_put_str(out_buf, pos, "r_mean");
        pos = mp_put_int(out_buf, pos, (s->r_sum + n / 2) / n);
        pos = mp_put_str(out_buf, pos, "g_mean");
        pos = mp_put_int(out_buf, pos, (s->g_sum + n / 2) / n);
        pos = mp_put_str(out_buf, pos, "b_mean");
        pos = mp_put_int(out_buf, pos, (s->b_sum + n / 2) / n);
    }
    pos = mp_put_str(out_buf, pos, "frame");
    out_buf[pos++] = 0x82;                        /* fixmap(2) */
    pos = mp_put_str(out_buf, pos, "w");
    pos = mp_put_int(out_buf, pos, w);
    pos = mp_put_str(out_buf, pos, "h");
    pos = mp_put_int(out_buf, pos, h);
    return pack(XUANSI_OFF(out_buf), pos);
}

/* ---- native seam init: place the host-touched buffers in the arena ---- */
#ifdef XUANSI_NATIVE_TEST
void xuansi_native_init(uint8_t *base)
{
    xuansi_lin_base = base;
    out_buf = base + LIN_OUT_OFF;
    args_slot = base + LIN_ARGS_OFF;
    frame_buf = base + LIN_FRAME_OFF;
}
#endif
