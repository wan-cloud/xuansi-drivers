/*
 * test_ledwatch.c — native unit test for the ledwatch module (stub host).
 *
 * Build & run (host toolchain, no wasm needed):
 *   cc -DXUANSI_NATIVE_TEST -Wall -Wextra -Werror -I src -I src/ledwatch \
 *      src/ledwatch/test_ledwatch.c -o build/test_ledwatch \
 *      && ./build/test_ledwatch
 *
 * Same seam as test_hello.c: the test includes ledwatch.c directly (one
 * translation unit), provides the `xuansi` host namespace as stubs, and
 * drives analyze() through the same u32-offset msgpack marshalling the
 * wasm host uses — the test owns the "linear memory" arena via
 * xuansi_native_init().
 *
 * The stub camera serves SYNTHETIC RGB565 frames staged by the test
 * (canvas API: clear/rect/circle/checkerboard -> stage_commit), counts
 * cam_frame_get calls, and raises exactly like the real host when
 * the camera is off — so discard_first's two-frame consumption is
 * asserted against the call counter, and the analyzed content is
 * asserted to come from the SECOND staged frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>

#include "ledwatch.c"   /* unity build: the module under test */

/* fix1 RED seam: the args-slot macro arrives with the fix; a test-local
 * fallback keeps this file compilable against the pre-fix module (the
 * high-water assertion then fails / the arena exhausts — the RED evidence) */
#ifndef ARGS_SLOT_SZ
#define ARGS_SLOT_SZ 128
#endif

/* ---- stub host state ---- */

static int s_camera_on;
static uint32_t s_frame_calls;               /* cam_frame_get counter */

/* staged frames (the stub serves them in order, last one repeats) */
#define STG_N 8
static uint8_t s_staged[STG_N][320u * 240u * 2u];
static int s_staged_n, s_staged_i;

/* trap emulation: the host refusal channel (same as test_hello) */
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

/* ---- the `xuansi` namespace stubs (only what ledwatch imports) ---- */

void xuansi_cam_frame_get(uint32_t *len_out, uint32_t buf_off, uint32_t cap,
                          uint32_t *w_out, uint32_t *h_out)
{
    if (!s_camera_on) stub_raise("xuansi.cam_frame_get: camera not on");
    uint32_t len = 320u * 240u * 2u;
    if (len > cap) stub_raise("xuansi.cam_frame_get: frame over cap");
    if (s_staged_n == 0) stub_raise("xuansi.cam_frame_get: stub: no staged frame");
    memcpy(xuansi_lin_base + buf_off,
           s_staged[s_staged_i < s_staged_n ? s_staged_i : s_staged_n - 1], len);
    if (s_staged_i < s_staged_n) s_staged_i++;
    s_frame_calls++;
    *len_out = len;
    *w_out = 320u;                /* geometry rides the call (abi=2) */
    *h_out = 240u;
}

/* ---- synthetic canvas -> staged frame ----
 * RGB565 pixel layout as ledwatch decodes it: hi byte first
 * (fb[2i] = R4..G3, fb[2i+1] = G2..B0) — the GC2145 DVP wire order.
 * Pure-color constants and their BT.601 luma (ledwatch's integer
 * approximation, 5/6-bit components replicated into 8 bits):
 *   white 0xFFFF -> (255,255,255) luma 255
 *   black 0x0000 -> (0,0,0)       luma 0
 *   red   0xF800 -> (255,0,0)     luma 76
 *   green 0x07E0 -> (0,255,0)     luma 149
 *   blue  0x001F -> (0,0,255)     luma 28
 */
static uint8_t s_canvas[320u * 240u * 2u];

#define C_WHITE 0xFFFFu
#define C_BLACK 0x0000u
#define C_RED   0xF800u
#define C_GREEN 0x07E0u
#define C_BLUE  0x001Fu

static void canvas_px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= 320 || y < 0 || y >= 240) return;
    uint32_t i = (uint32_t)(y * 320 + x) * 2u;
    s_canvas[i] = (uint8_t)(c >> 8);
    s_canvas[i + 1] = (uint8_t)(c & 0xFF);
}

static void canvas_clear(uint16_t c)
{
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) canvas_px(x, y, c);
}

static void canvas_rect(uint16_t c, int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) canvas_px(x, y, c);
}

static void canvas_circle(uint16_t c, int cx, int cy, int r)
{
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                canvas_px(x, y, c);
}

static void canvas_checker(uint16_t on, uint16_t off)
{
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            canvas_px(x, y, ((x + y) & 1) == 0 ? on : off);
}

static void stage_reset(void)
{
    s_staged_n = 0;
    s_staged_i = 0;
}

static void stage_commit(void)
{
    if (s_staged_n >= STG_N) {
        fprintf(stderr, "FATAL: stage overflow\n");
        exit(2);
    }
    memcpy(s_staged[s_staged_n], s_canvas, sizeof s_canvas);
    s_staged_n++;
    s_staged_i = 0;
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
    long x, y, w, h, area, luma_mean, r_mean, g_mean, b_mean;
} region_t;

typedef struct {
    int n;
    region_t r[40];
    long frame_w, frame_h;
} result_t;

static const uint8_t *s_res_p;
static uint32_t s_res_len, s_res_pos;

static int rd_u8(uint8_t *out)
{
    if (s_res_pos >= s_res_len) return -1;
    *out = s_res_p[s_res_pos++];
    return 0;
}

static int rd_i64(int64_t *out)   /* int family: fixint + u8/u16/u32 */
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

/* fixstr key; on mismatch the read position is RESTORED (map iteration
 * tries keys in turn — a failed try must not consume the key bytes) */
static int rd_key(const char *want)
{
    uint32_t save = s_res_pos;
    uint8_t t;
    if (rd_u8(&t) != 0) return -1;
    if ((t & 0xe0) != 0xa0) { s_res_pos = save; return -1; }
    uint32_t n = t & 0x1f;
    if (s_res_pos + n > s_res_len) { s_res_pos = save; return -1; }
    int ok = (strlen(want) == n)
          && (memcmp(s_res_p + s_res_pos, want, n) == 0);
    if (!ok) { s_res_pos = save; return -1; }
    s_res_pos += n;
    return 0;
}

static int rd_region(region_t *r)   /* fixmap(9) of the nine frozen fields */
{
    uint8_t t;
    if (rd_u8(&t) != 0 || (t & 0xf0) != 0x80 || (t & 0x0f) != 9) return -1;
    memset(r, 0, sizeof *r);
    static const char *fields[9] = { "x", "y", "w", "h", "area", "luma_mean",
                                     "r_mean", "g_mean", "b_mean" };
    for (int i = 0; i < 9; i++) {
        if (rd_key(fields[i]) != 0) return -1;
        int64_t v;
        if (rd_i64(&v) != 0) return -1;
        long *f = i == 0 ? &r->x : i == 1 ? &r->y : i == 2 ? &r->w
                : i == 3 ? &r->h : i == 4 ? &r->area : i == 5 ? &r->luma_mean
                : i == 6 ? &r->r_mean : i == 7 ? &r->g_mean : &r->b_mean;
        *f = (long)v;
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
    if (rd_u8(&t) != 0 || (t & 0xf0) != 0x80 || (t & 0x0f) != 2)
        return -1;                                  /* fixmap(2) root */
    int64_t v;
    int have_frame = 0, have_regions = 0;
    for (int k = 0; k < 2; k++) {
        if (rd_key("frame") == 0) {
            if (rd_u8(&t) != 0 || (t & 0xf0) != 0x80 || (t & 0x0f) != 2)
                return -1;                          /* fixmap(2) {w,h} */
            if (rd_key("w") != 0 || rd_i64(&v) != 0) return -1;
            res->frame_w = (long)v;
            if (rd_key("h") != 0 || rd_i64(&v) != 0) return -1;
            res->frame_h = (long)v;
            have_frame = 1;
        } else if (rd_key("regions") == 0) {
            if (rd_u8(&t) != 0) return -1;
            uint32_t n;
            if ((t & 0xf0) == 0x90) n = t & 0x0f;
            else if (t == 0xdc) {
                uint8_t b0, b1;
                if (rd_u8(&b0) != 0 || rd_u8(&b1) != 0) return -1;
                n = ((uint32_t)b0 << 8) | b1;
            } else return -1;
            if (n > 40) return -1;
            for (uint32_t i = 0; i < n; i++)
                if (rd_region(&res->r[i]) != 0) return -1;
            res->n = (int)n;
            have_regions = 1;
        } else {
            return -1;
        }
    }
    return (have_frame && have_regions) ? 0 : -1;
}

/* the wasm host's arg-writing step: alloc, then write msgpack bytes */
static uint32_t host_write_args(const uint8_t *bytes, uint32_t n)
{
    uint32_t off = xuansi_alloc(n);
    if (off == 0) { fprintf(stderr, "FATAL: alloc refused\n"); exit(2); }
    memcpy(arena + off, bytes, n);
    return off;
}

/* cfg msgpack builder from a compact spec (values parsed from the spec
 * itself — ints are encoded at their natural msgpack width so the
 * module's reader sees fixint/u8/u16/u32 just like the real host):
 *   "threshold=i:200,min_area=i:16,discard_first=b:0"
 */
static uint32_t build_args(uint8_t *buf, const char *kvs)
{
    uint32_t pos = 0, nkv = 0;
    const char *p = kvs;
    while (*p) { nkv++; while (*p && *p != ',') p++; if (*p) p++; }
    if (nkv > 15) { fprintf(stderr, "FATAL: cfg nkv\n"); exit(2); }
    buf[pos++] = 0x91;                              /* fixarray(1) */
    buf[pos++] = (uint8_t)(0x80 | nkv);             /* fixmap(nkv) */
    p = kvs;
    for (uint32_t i = 0; i < nkv; i++) {
        const char *eq = strchr(p, '=');
        const char *colon = strchr(p, ':');
        const char *comma = strchr(p, ',');
        if (!eq || !colon || eq > colon || (comma && colon > comma)) {
            fprintf(stderr, "FATAL: cfg spec syntax\n"); exit(2);
        }
        size_t klen = (size_t)(eq - p);
        const char *vend = comma ? comma : (p + strlen(p));
        size_t vlen = (size_t)(vend - colon - 1);   /* "key=i:VAL" -> VAL */
        char val[32];
        if (klen > 31 || vlen >= sizeof val) {
            fprintf(stderr, "FATAL: cfg len\n"); exit(2);
        }
        buf[pos++] = (uint8_t)(0xa0 | klen);
        memcpy(buf + pos, p, klen); pos += (uint32_t)klen;
        memcpy(val, colon + 1, vlen); val[vlen] = 0;
        if (*(eq + 1) == 'i') {
            long iv = strtol(val, NULL, 10);
            if (iv >= 0 && iv < 128) buf[pos++] = (uint8_t)iv;
            else if (iv < 0x100) { buf[pos++] = 0xcc; buf[pos++] = (uint8_t)iv; }
            else if (iv < 0x10000) {
                buf[pos++] = 0xcd;
                buf[pos++] = (uint8_t)(iv >> 8); buf[pos++] = (uint8_t)iv;
            } else {
                buf[pos++] = 0xce;
                buf[pos++] = (uint8_t)(iv >> 24); buf[pos++] = (uint8_t)(iv >> 16);
                buf[pos++] = (uint8_t)(iv >> 8);  buf[pos++] = (uint8_t)iv;
            }
        } else if (*(eq + 1) == 'b') {
            buf[pos++] = (uint8_t)(strcmp(val, "1") == 0 ? 0xc3 : 0xc2);
        } else {
            fprintf(stderr, "FATAL: cfg kind\n"); exit(2);
        }
        p = comma ? comma + 1 : p + strlen(p);
    }
    return pos;
}

/* run analyze() with a cfg spec (NULL/"" = no args at all) */
static uint64_t call_analyze(const char *kvs)
{
    uint8_t abuf[128];
    uint32_t n = 0, off = 0;
    if (kvs && kvs[0]) {
        n = build_args(abuf, kvs);
        off = host_write_args(abuf, n);
    }
    return analyze(off, n);
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
static void reset_state(int camera_on)
{
    s_camera_on = camera_on;
    s_frame_calls = 0;
    s_staged_n = 0;
    s_staged_i = 0;
    memset(arena, 0, sizeof arena);
    xuansi_native_init(arena);
}

int main(void)
{
    result_t res;
    char detail[256];

    reset_state(1);

    /* ---- T01 abi + allocator ---- */
    check(xuansi_abi() == 1, "T01 xuansi_abi()==1 (the B15 recarve)", "");
    check(xuansi_alloc(0) == 0, "T01b alloc(0) refused", "");
    check(xuansi_alloc(200) == LIN_HEAP_OFF + 8,
          "T01c large alloc -> heap_buf-based bump (P3-T12 rider: no bare "
          ".rodata-range offsets)", "");

    /* ---- T02 single rect: exact bbox/area/means, defaults ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 20, 30, 60, 40);       /* area 2400 */
        stage_commit();
        uint64_t r = call_analyze("");
        int ok = res_decode(r, &res) == 0
              && res.frame_w == 320 && res.frame_h == 240
              && res.n == 1
              && res.r[0].x == 20 && res.r[0].y == 30
              && res.r[0].w == 60 && res.r[0].h == 40
              && res.r[0].area == 2400
              && res.r[0].r_mean == 255 && res.r[0].g_mean == 255
              && res.r[0].b_mean == 255 && res.r[0].luma_mean == 255;
        snprintf(detail, sizeof detail,
                 "n=%d r0=(%ld,%ld,%ld,%ld) a=%ld rgb=(%ld,%ld,%ld) l=%ld",
                 res.n, res.r[0].x, res.r[0].y, res.r[0].w, res.r[0].h,
                 res.r[0].area, res.r[0].r_mean, res.r[0].g_mean,
                 res.r[0].b_mean, res.r[0].luma_mean);
        check(ok, "T02 single white rect exact (defaults)", detail);
        check(s_frame_calls == 2, "T02b discard_first default consumes 2 frames",
              "");
    }

    /* ---- T03 two rects separated: both found, sorted desc by area ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 200, 150, 30, 30);     /* area 900 */
        canvas_rect(C_WHITE, 20, 30, 60, 40);       /* area 2400 */
        stage_commit();
        uint64_t r = call_analyze("");
        int ok = res_decode(r, &res) == 0 && res.n == 2
              && res.r[0].area == 2400 && res.r[0].x == 20
              && res.r[1].area == 900 && res.r[1].x == 200;
        check(ok, "T03 dual targets separated, sorted desc by area", "");
    }

    /* ---- T04 circle: area ~ pi*r^2, bbox ~ bounding square ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_circle(C_WHITE, 160, 120, 40);
        stage_commit();
        uint64_t r = call_analyze("");
        int ok = res_decode(r, &res) == 0 && res.n == 1
              && labs(res.r[0].area - 5026) <= 160
              && res.r[0].x >= 119 && res.r[0].x <= 121
              && res.r[0].y >= 79 && res.r[0].y <= 81
              && res.r[0].w >= 79 && res.r[0].w <= 83
              && res.r[0].h >= 79 && res.r[0].h <= 83;
        snprintf(detail, sizeof detail, "area=%ld bbox=(%ld,%ld,%ld,%ld)",
                 res.r[0].area, res.r[0].x, res.r[0].y, res.r[0].w, res.r[0].h);
        check(ok, "T04 filled circle r=40 (area/bbox tolerances)", detail);
    }

    /* ---- T05 empty all-dark: zero regions ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        stage_commit();
        uint64_t r = call_analyze("");
        int ok = res_decode(r, &res) == 0 && res.n == 0
              && res.frame_w == 320 && res.frame_h == 240;
        check(ok, "T05 empty all-dark -> regions==[]", "");
    }

    /* ---- T06 full all-bright: one region, whole frame ---- */
    {
        stage_reset();
        canvas_clear(C_WHITE);
        stage_commit();
        uint64_t r = call_analyze("");
        int ok = res_decode(r, &res) == 0 && res.n == 1
              && res.r[0].x == 0 && res.r[0].y == 0
              && res.r[0].w == 320 && res.r[0].h == 240
              && res.r[0].area == 320L * 240L;
        snprintf(detail, sizeof detail, "area=%ld", res.r[0].area);
        check(ok, "T06 full all-bright -> one whole-frame region", detail);
    }

    /* ---- T07 edge-touching targets ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 0, 0, 50, 50);
        canvas_rect(C_WHITE, 270, 190, 50, 50);     /* touches (319,239) */
        stage_commit();
        uint64_t r = call_analyze("");
        int ok = res_decode(r, &res) == 0 && res.n == 2
              && res.r[0].x + res.r[0].w <= 320 && res.r[0].y + res.r[0].h <= 240
              && res.r[1].x + res.r[1].w <= 320 && res.r[1].y + res.r[1].h <= 240
              && ((res.r[0].x == 0 && res.r[0].y == 0)
               || (res.r[1].x == 0 && res.r[1].y == 0))
              && ((res.r[0].x == 270 && res.r[0].y == 190)
               || (res.r[1].x == 270 && res.r[1].y == 190));
        check(ok, "T07 edge-touching targets, bbox in-bounds", "");
    }

    /* ---- T08 min_area filter (boundary: area == min_area kept) ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 10, 10, 4, 4);         /* area 16 */
        stage_commit();
        uint64_t r = call_analyze("min_area=i:17");
        int dec = res_decode(r, &res);
        snprintf(detail, sizeof detail, "dec=%d n=%d", dec, res.n);
        check(dec == 0 && res.n == 0,
              "T08a area 16 dropped at min_area=17", detail);
        r = call_analyze("min_area=i:16");
        check(res_decode(r, &res) == 0 && res.n == 1 && res.r[0].area == 16,
              "T08b area 16 kept at min_area=16", "");
    }

    /* ---- T09 colored means: pure R/G/B with low threshold ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_RED, 10, 10, 20, 20);
        canvas_rect(C_GREEN, 100, 10, 20, 20);
        canvas_rect(C_BLUE, 190, 10, 20, 20);
        stage_commit();
        uint64_t r = call_analyze("threshold=i:20");
        int ok = res_decode(r, &res) == 0 && res.n == 3;
        long rr = -1, gg = -1, bb = -1, lr = -1, lg = -1, lb = -1;
        for (int i = 0; i < res.n; i++) {
            if (res.r[i].r_mean > 200 && res.r[i].g_mean < 10) {
                rr = res.r[i].r_mean; lr = res.r[i].luma_mean;
            } else if (res.r[i].g_mean > 200) {
                gg = res.r[i].g_mean; lg = res.r[i].luma_mean;
            } else if (res.r[i].b_mean > 200) {
                bb = res.r[i].b_mean; lb = res.r[i].luma_mean;
            }
        }
        snprintf(detail, sizeof detail, "r=%ld l=%ld g=%ld l=%ld b=%ld l=%ld",
                 rr, lr, gg, lg, bb, lb);
        /* BT.601 integer approx with replicated 8-bit components:
         * red 77*255>>8=76, green 150*255>>8=149, blue 29*255>>8=28 */
        check(ok && rr == 255 && lr == 76 && gg == 255 && lg == 149
              && bb == 255 && lb == 28,
              "T09 pure R/G/B means + BT.601 luma (76/149/28)", detail);
    }

    /* ---- T10 threshold boundary: gray luma 100 vs threshold 100/101 ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        /* r5=12 g6=25 b5=12 -> r8=99 g8=101 b8=99 -> luma 100 exactly */
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 320; x++)
                canvas_px(x, y, (uint16_t)((12u << 11) | (25u << 5) | 12u));
        stage_commit();
        uint64_t r = call_analyze("threshold=i:100");
        int dec1 = res_decode(r, &res);
        snprintf(detail, sizeof detail, "dec=%d n=%d a=%ld l=%ld",
                 dec1, res.n, res.r[0].area, res.r[0].luma_mean);
        int ok1 = dec1 == 0 && res.n == 1
               && res.r[0].area == 320L * 240L && res.r[0].luma_mean == 100;
        r = call_analyze("threshold=i:101");
        int ok2 = res_decode(r, &res) == 0 && res.n == 0;
        check(ok1 && ok2, "T10 threshold >= semantics at gray luma 100", detail);
    }

    /* ---- T11 discard_first=false consumes one frame ---- */
    {
        stage_reset();
        s_frame_calls = 0;
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 0, 0, 20, 20);
        stage_commit();                             /* frame 1 */
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 100, 100, 20, 20);
        stage_commit();                             /* frame 2 */
        uint64_t r = call_analyze("discard_first=b:0");
        int ok = res_decode(r, &res) == 0 && res.n == 1
              && res.r[0].x == 0 && res.r[0].y == 0
              && s_frame_calls == 1;
        check(ok, "T11 discard_first=false: 1 frame, first content", "");
    }

    /* ---- T12 discard_first=true analyzes the SECOND frame ---- */
    {
        stage_reset();
        s_frame_calls = 0;
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 0, 0, 20, 20);
        stage_commit();                             /* discarded frame */
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 100, 100, 20, 20);
        stage_commit();                             /* analyzed frame */
        uint64_t r = call_analyze("discard_first=b:1");
        int ok = res_decode(r, &res) == 0 && res.n == 1
              && res.r[0].x == 100 && res.r[0].y == 100
              && s_frame_calls == 2;
        check(ok, "T12 discard_first=true: 2 frames, second content", "");
    }

    /* ---- T13 max_regions cap + desc order ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 10, 10, 30, 30);       /* 900 */
        canvas_rect(C_WHITE, 100, 10, 60, 40);      /* 2400 */
        canvas_rect(C_WHITE, 200, 10, 10, 10);      /* 100 */
        stage_commit();
        uint64_t r = call_analyze("max_regions=i:2");
        int ok = res_decode(r, &res) == 0 && res.n == 2
              && res.r[0].area == 2400 && res.r[1].area == 900;
        check(ok, "T13 max_regions=2 caps + keeps largest, desc", "");
    }

    /* ---- T14 checkerboard degenerate: 38400 area-1 labels ---- */
    {
        stage_reset();
        canvas_checker(C_WHITE, C_BLACK);
        stage_commit();
        uint64_t r = call_analyze("");              /* min_area 16 drops all */
        int ok = res_decode(r, &res) == 0 && res.n == 0;
        check(ok, "T14a checkerboard 38400 area-1 regions all < min_area", "");
        r = call_analyze("min_area=i:1,max_regions=i:8");
        ok = res_decode(r, &res) == 0 && res.n == 8
          && res.r[0].area == 1 && res.r[7].area == 1;
        check(ok, "T14b checkerboard min_area=1: 8 area-1 regions (u16 "
              "label space safe)", "");
    }

    /* ---- T15 camera off: host refusal propagates ---- */
    {
        s_camera_on = 0;
        EXPECT_TRAP(call_analyze(""), "T15 camera-off refusal propagates");
        s_camera_on = 1;
    }

    /* ---- T16 unknown cfg keys tolerated (forward compat) ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 20, 30, 60, 40);
        stage_commit();
        uint64_t r = call_analyze("bogus=i:7");
        int ok = res_decode(r, &res) == 0 && res.n == 1 && res.r[0].area == 2400;
        check(ok, "T16 unknown cfg key ignored", "");
    }

    /* ---- T17 uint16 arg width decode (200 needs 0xcd) ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 20, 30, 60, 40);
        stage_commit();
        /* threshold=200 >127 -> builder emits uint16 0xcd 0x00 0xc8 */
        uint64_t r = call_analyze("threshold=i:200");
        check(res_decode(r, &res) == 0 && res.n == 1 && res.r[0].area == 2400,
              "T17 uint16-encoded cfg int decoded", "");
    }

    /* ---- T18 (fix1) merged-label frame + min_area=0: no div-zero trap ----
     * U-shape forces a pass-1 union: raster order creates two labels (the
     * arms' tops), the bottom bar merges them — the absorbed label becomes
     * a NON-ROOT whose lw_area stays 0 (a "ghost"). With min_area=0 the old
     * top-N filter (a < mina only) let ghosts in; stats never touched them
     * (no pixel resolves to a non-root) and the encode divided by n=0 ->
     * wasm i32 div-zero trap (plus bbox underflow). */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 10, 10, 15, 60);    /* left arm  x=10..24 */
        canvas_rect(C_WHITE, 50, 10, 15, 60);    /* right arm x=50..64 */
        canvas_rect(C_WHITE, 10, 55, 55, 15);    /* bottom bar — merges */
        stage_commit();
        uint64_t r = call_analyze("min_area=i:0");
        int dec = res_decode(r, &res);
        int ok = dec == 0 && res.n == 1
              && res.r[0].x == 10 && res.r[0].y == 10
              && res.r[0].w == 55 && res.r[0].h == 60
              && res.r[0].area == 2175        /* 900+900+825-225-225 */
              && res.r[0].luma_mean == 255;
        snprintf(detail, sizeof detail,
                 "dec=%d n=%d bbox=(%ld,%ld,%ld,%ld) a=%ld",
                 dec, res.n, res.r[0].x, res.r[0].y, res.r[0].w, res.r[0].h,
                 res.r[0].area);
        check(ok, "T18 U-merged ghost label + min_area=0: single region, "
              "no trap", detail);
        /* min_area=0 on separated targets: the "no filter" idiom returns
         * every component, still no ghost */
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 20, 30, 60, 40);    /* 2400 */
        canvas_rect(C_WHITE, 200, 150, 30, 30);  /* 900 */
        stage_commit();
        r = call_analyze("min_area=i:0");
        ok = res_decode(r, &res) == 0 && res.n == 2
          && res.r[0].area == 2400 && res.r[1].area == 900;
        check(ok, "T18b min_area=0 separated targets: both, no trap", "");
    }

    /* ---- T19 (fix1) args-slot budget: polling calls must not exhaust the
     * bump arena — small args (<=ARGS_SLOT_SZ) reuse a fixed slot; the
     * bump high-water must be stable across 1000 cfg'd calls ---- */
    {
        stage_reset();
        canvas_clear(C_BLACK);
        canvas_rect(C_WHITE, 20, 30, 60, 40);
        stage_commit();
        uint32_t hw0 = heap_off;         /* module static: same TU */
        uint64_t r = 0;
        int i;
        for (i = 0; i < 1000; i++)
            r = call_analyze("threshold=i:120,min_area=i:16");
        int dec = res_decode(r, &res);
        snprintf(detail, sizeof detail, "hw %u->%u dec=%d n=%d",
                 hw0, heap_off, dec, res.n);
        check(dec == 0 && res.n == 1 && res.r[0].area == 2400
              && heap_off == hw0,
              "T19 1000 cfg'd analyze calls: results intact, alloc "
              "high-water stable", detail);
        /* large args (> slot) still take the bump arena — one-shot path */
        uint32_t hw1 = heap_off;
        uint8_t big[256];
        uint32_t n = build_args(big,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=i:1,"   /* 6 x 31-char keys */
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb=i:1,"
            "cccccccccccccccccccccccccccccc=i:1,"
            "dddddddddddddddddddddddddddddd=i:1,"
            "eeeeeeeeeeeeeeeeeeeeeeeeeeeeee=i:1,"
            "ffffffffffffffffffffffffffffff=i:1");
        uint32_t off = host_write_args(big, n);
        r = analyze(off, n);
        check(n > ARGS_SLOT_SZ && heap_off > hw1
              && res_decode(r, &res) == 0 && res.n == 1,
              "T19b args > slot still bump (one-shot), unknown keys ignored",
              "");
    }

    printf("---- test_ledwatch: %d checks, %d fail ----\n", checks, fails);
    return fails ? 1 : 0;
}
